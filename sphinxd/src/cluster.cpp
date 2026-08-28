// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sphinx/cluster.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include "MurmurHash3.h"

namespace sphinx::cluster {
namespace {

constexpr uint32_t kMurmurSeed = 1;

// 使用 32 位 MurmurHash3 计算字符串哈希值
uint32_t hash_string(std::string_view value) {
  uint32_t hash = 0;
  MurmurHash3_x86_32(value.data(), static_cast<int>(value.size()), kMurmurSeed, &hash);
  return hash;
}

// 检查字符串中是否包含空白字符
bool contains_whitespace(std::string_view value) {
  for (const char character : value) {
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      return true;
    }
  }
  return false;
}

// 校验单个节点的配置属性合法性
void validate_node(const Node& node) {
  if (node.host.empty() || node.host.find(':') != std::string::npos ||
      contains_whitespace(node.host) || node.port == 0) {
    throw std::invalid_argument("invalid cluster node");
  }
}

// 解析并验证端口号字符串（必须是 1~65535 范围内的十进制整数）
uint16_t parse_port(std::string_view port_text) {
  if (port_text.empty()) {
    throw std::invalid_argument("cluster node is missing a port");
  }

  uint32_t port = 0;
  for (const char character : port_text) {
    if (character < '0' || character > '9') {
      throw std::invalid_argument("cluster node port must be decimal");
    }
    port = port * 10U + static_cast<uint32_t>(character - '0');
    if (port > std::numeric_limits<uint16_t>::max()) {
      throw std::invalid_argument("cluster node port is out of range");
    }
  }

  if (port == 0) {
    throw std::invalid_argument("cluster node port is out of range");
  }

  return static_cast<uint16_t>(port);
}

// 校验整个节点列表是否有效且不包含重复节点
void validate_nodes(const std::vector<Node>& nodes) {
  std::unordered_set<std::string> ids;
  ids.reserve(nodes.size());

  for (const auto& node : nodes) {
    validate_node(node);
    if (!ids.emplace(node.id()).second) {
      throw std::invalid_argument("duplicate cluster node");
    }
  }
}

}  // namespace

// 返回节点全局唯一标识符（格式为 host:port）
std::string Node::id() const {
  return host + ":" + std::to_string(port);
}

// 节点相等性比较运算符
bool Node::operator==(const Node& other) const {
  return host == other.host && port == other.port;
}

// 解析形如 "host1:port1,host2:port2" 的集群节点规格字符串
std::vector<Node> parse_nodes(std::string_view specification) {
  // 1. 基础非空与空白字符检查
  if (specification.empty() || contains_whitespace(specification)) {
    throw std::invalid_argument(
        "cluster node configuration must be non-empty and contain no whitespace");
  }

  std::vector<Node> nodes;
  std::unordered_set<std::string> ids;
  size_t begin = 0;

  // 2. 按逗号分割并逐个解析各节点端点
  while (begin <= specification.size()) {
    const size_t end = specification.find(',', begin);
    const size_t length =
        end == std::string_view::npos ? specification.size() - begin : end - begin;
    const std::string_view endpoint = specification.substr(begin, length);
    if (endpoint.empty()) {
      throw std::invalid_argument("cluster node configuration contains an empty endpoint");
    }

    // 校验 host:port 分隔冒号
    const size_t colon = endpoint.find(':');
    if (colon == std::string_view::npos ||
        endpoint.find(':', colon + 1) != std::string_view::npos) {
      throw std::invalid_argument("cluster node must use host:port syntax");
    }

    const std::string_view host = endpoint.substr(0, colon);
    if (host.empty()) {
      throw std::invalid_argument("cluster node host must be non-empty");
    }

    // 构造节点并验证重复性
    const Node node{std::string(host), parse_port(endpoint.substr(colon + 1))};
    validate_node(node);
    if (!ids.emplace(node.id()).second) {
      throw std::invalid_argument("duplicate cluster node");
    }
    nodes.push_back(node);

    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }

  return nodes;
}

// 基于实体节点列表构建一致性哈希环
ConsistentHashRing::ConsistentHashRing(std::vector<Node> nodes) : _nodes(std::move(nodes)) {
  // 1. 校验输入节点集合
  validate_nodes(_nodes);

  // 2. 为每个节点生成对应数量的虚拟节点并计算其哈希位置
  _entries.reserve(_nodes.size() * kVirtualNodesPerNode);
  for (const auto& node : _nodes) {
    const std::string node_id = node.id();
    for (uint32_t virtual_index = 0; virtual_index < kVirtualNodesPerNode; virtual_index++) {
      const std::string virtual_id = node_id + "#" + std::to_string(virtual_index);
      _entries.push_back(RingEntry{hash_string(virtual_id), node_id, virtual_index, node});
    }
  }

  // 3. 将虚拟节点按哈希值升序排序（遇哈希冲突时按 node_id 和 virtual_index 稳定排序）
  std::sort(_entries.begin(), _entries.end(), [](const RingEntry& left, const RingEntry& right) {
    if (left.hash != right.hash) {
      return left.hash < right.hash;
    }
    if (left.node_id != right.node_id) {
      return left.node_id < right.node_id;
    }
    return left.virtual_index < right.virtual_index;
  });
}

// 基于节点字符串规格构造哈希环
ConsistentHashRing::ConsistentHashRing(std::string_view specification)
    : ConsistentHashRing(parse_nodes(specification)) {
}

// 根据键的一致性哈希路由到负责的目标节点
Node ConsistentHashRing::route(std::string_view key) const {
  // 1. 校验哈希环非空
  if (_entries.empty()) {
    throw std::runtime_error("cannot route a key with an empty cluster");
  }

  // 2. 计算 key 的 MurmurHash3 哈希值
  const uint32_t key_hash = hash_string(key);

  // 3. 在哈希环上二分查找第一个哈希值 >= key_hash 的虚拟节点
  const auto it =
      std::lower_bound(_entries.begin(), _entries.end(), key_hash,
                       [](const RingEntry& entry, uint32_t hash) { return entry.hash < hash; });

  // 4. 若超出最大哈希值，则顺时针环绕回环首节点
  return (it == _entries.end() ? _entries.front() : *it).node;
}

// 获取实体节点列表引用
const std::vector<Node>& ConsistentHashRing::nodes() const {
  return _nodes;
}

// 获取哈希环上排序后的所有虚拟节点条目
const std::vector<RingEntry>& ConsistentHashRing::entries() const {
  return _entries;
}

}  // namespace sphinx::cluster
