// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sphinx/cluster.h"

#include "MurmurHash3.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace sphinx::cluster {
namespace {

constexpr uint32_t kMurmurSeed = 1;

uint32_t
hash_string(std::string_view value)
{
  uint32_t hash = 0;
  MurmurHash3_x86_32(value.data(), static_cast<int>(value.size()), kMurmurSeed, &hash);
  return hash;
}

bool
contains_whitespace(std::string_view value)
{
  for (const char character : value) {
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      return true;
    }
  }
  return false;
}

void
validate_node(const Node& node)
{
  if (node.host.empty() || node.host.find(':') != std::string::npos ||
      contains_whitespace(node.host) || node.port == 0) {
    throw std::invalid_argument("invalid cluster node");
  }
}

uint16_t
parse_port(std::string_view port_text)
{
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

void
validate_nodes(const std::vector<Node>& nodes)
{
  std::unordered_set<std::string> ids;
  ids.reserve(nodes.size());
  for (const auto& node : nodes) {
    validate_node(node);
    if (!ids.emplace(node.id()).second) {
      throw std::invalid_argument("duplicate cluster node");
    }
  }
}

} // namespace

std::string
Node::id() const
{
  return host + ":" + std::to_string(port);
}

bool
Node::operator==(const Node& other) const
{
  return host == other.host && port == other.port;
}

std::vector<Node>
parse_nodes(std::string_view specification)
{
  if (specification.empty() || contains_whitespace(specification)) {
    throw std::invalid_argument(
      "cluster node configuration must be non-empty and contain no whitespace");
  }

  std::vector<Node> nodes;
  std::unordered_set<std::string> ids;
  size_t begin = 0;
  while (begin <= specification.size()) {
    const size_t end = specification.find(',', begin);
    const size_t length =
      end == std::string_view::npos ? specification.size() - begin : end - begin;
    const std::string_view endpoint = specification.substr(begin, length);
    if (endpoint.empty()) {
      throw std::invalid_argument("cluster node configuration contains an empty endpoint");
    }

    const size_t colon = endpoint.find(':');
    if (colon == std::string_view::npos ||
        endpoint.find(':', colon + 1) != std::string_view::npos) {
      throw std::invalid_argument("cluster node must use host:port syntax");
    }

    const std::string_view host = endpoint.substr(0, colon);
    if (host.empty()) {
      throw std::invalid_argument("cluster node host must be non-empty");
    }
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

ConsistentHashRing::ConsistentHashRing(std::vector<Node> nodes)
  : _nodes(std::move(nodes))
{
  validate_nodes(_nodes);
  _entries.reserve(_nodes.size() * kVirtualNodesPerNode);
  for (const auto& node : _nodes) {
    const std::string node_id = node.id();
    for (uint32_t virtual_index = 0; virtual_index < kVirtualNodesPerNode; virtual_index++) {
      const std::string virtual_id = node_id + "#" + std::to_string(virtual_index);
      _entries.push_back(RingEntry{hash_string(virtual_id), node_id, virtual_index, node});
    }
  }

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

ConsistentHashRing::ConsistentHashRing(std::string_view specification)
  : ConsistentHashRing(parse_nodes(specification))
{
}

Node
ConsistentHashRing::route(std::string_view key) const
{
  if (_entries.empty()) {
    throw std::runtime_error("cannot route a key with an empty cluster");
  }

  const uint32_t key_hash = hash_string(key);
  const auto it = std::lower_bound(
    _entries.begin(), _entries.end(), key_hash, [](const RingEntry& entry, uint32_t hash) {
      return entry.hash < hash;
    });
  return (it == _entries.end() ? _entries.front() : *it).node;
}

const std::vector<Node>&
ConsistentHashRing::nodes() const
{
  return _nodes;
}

const std::vector<RingEntry>&
ConsistentHashRing::entries() const
{
  return _entries;
}

} // namespace sphinx::cluster
