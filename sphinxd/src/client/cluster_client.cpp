// SPDX-License-Identifier: Apache-2.0
#include <sphinx/cluster_client.h>

#include <charconv>
#include <chrono>
#include <limits>
#include <unordered_set>
#include <utility>

#include "cluster_transport.h"

namespace sphinx {
namespace {

// 抛出包含节点标识与具体细节的 ClientError 异常
[[noreturn]] void throw_node_error(std::string_view target, std::string_view detail) {
  throw ClientError{std::string{"node "} + std::string{target} + ": " + std::string{detail}};
}

// 为字符串添加单引号包裹
std::string quote(std::string_view value) { return "'" + std::string{value} + "'"; }

// 解析十进制整数字符串，解析失败时抛出错误
uint64_t parse_decimal(std::string_view target, const char* field, std::string_view value) {
  uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);

  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw_node_error(target, std::string{"invalid "} + field + " in response");
  }

  return result;
}

// 拼接形如 "<command> <key>\r\n" 的请求字符串
std::string make_key_request(std::string_view command, std::string_view key) {
  return std::string{command} + ' ' + std::string{key} + "\r\n";
}

// 校验超时时间必须为正数
void validate_timeout(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0 || timeout.count() > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("cluster client timeout must be in 1..INT_MAX milliseconds");
  }
}

struct ValueHeader {
  std::string key;
  std::size_t value_size = 0;
};

// 解析 get 响应行头部：VALUE <key> <flags> <bytes>\r\n。
ValueHeader parse_value_header(std::string_view target, const std::string& response) {
  // 1. 响应行前缀与后缀格式检查
  if (response.size() < 10 || response.compare(0, 6, "VALUE ") != 0 ||
      response.compare(response.size() - 2, 2, "\r\n") != 0) {
    throw_node_error(target, "unexpected get response " + quote(response));
  }

  // 2. 字段空格分隔符拆分
  const auto body = response.substr(6, response.size() - 8);
  const auto first_space = body.find(' ');
  const auto second_space = body.find(' ', first_space + 1);

  if (first_space == std::string_view::npos || first_space == 0 ||
      second_space == std::string_view::npos || second_space == first_space + 1) {
    throw_node_error(target, "malformed get header " + quote(response));
  }

  ValueHeader header;
  header.key = std::string{body.substr(0, first_space)};

  // 4. 解析 flags 字段
  const auto flags =
      parse_decimal(target, "flags", body.substr(first_space + 1, second_space - first_space - 1));
  if (flags > std::numeric_limits<uint32_t>::max()) {
    throw_node_error(target, "flags are out of range in response");
  }

  // 5. 解析载荷字节大小字段
  const auto length = body.substr(second_space + 1);
  const auto bytes = parse_decimal(target, "value length", length);
  if (bytes > std::numeric_limits<size_t>::max()) {
    throw_node_error(target, "value length is too large");
  }

  header.value_size = static_cast<std::size_t>(bytes);
  return header;
}

}  // namespace

// 单个 Memcached 节点连接客户端封装
class ClusterClient::MemcachedConnection final {
 public:
  MemcachedConnection(const Node& node, std::chrono::milliseconds timeout)
      : _transport{node, timeout} {}

  MemcachedConnection(const MemcachedConnection&) = delete;
  MemcachedConnection& operator=(const MemcachedConnection&) = delete;

  // 执行 set 写入命令
  bool set(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) {
    _transport.begin_operation();
    // 1. 构建标准 set 请求帧
    std::string request{"set "};
    request.reserve(32 + key.size() + value.size());
    request += key;
    request += " 0 ";
    request += std::to_string(ttl_seconds);
    request += " ";
    request += std::to_string(value.size());
    request += "\r\n";
    request += value;
    request += "\r\n";

    // 2. 发送请求并读取响应
    _transport.write_all(request);
    const auto response = _transport.read_line();

    if (response != "STORED\r\n") {
      throw_node_error(_transport.target(), "unexpected set response " + quote(response));
    }

    return true;
  }

  // 执行单 key get 查询。
  std::optional<std::string> get(std::string_view key) {
    auto values = get_many({std::string{key}});
    return std::move(values.front());
  }

  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) {
    if (keys.empty()) {
      return {};
    }
    _transport.begin_operation();
    std::string request{"get"};
    for (const auto& key : keys) {
      request.push_back(' ');
      request += key;
    }
    request += "\r\n";
    _transport.write_all(request);

    std::unordered_map<std::string, std::vector<std::size_t>> requested_positions;
    requested_positions.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
      requested_positions[keys[index]].push_back(index);
    }
    std::unordered_set<std::string> returned_keys;
    returned_keys.reserve(keys.size());
    std::vector<std::optional<std::string>> values(keys.size());

    while (true) {
      const auto line = _transport.read_line();
      if (line == "END\r\n") {
        return values;
      }

      const ValueHeader header = parse_value_header(_transport.target(), line);
      const auto requested = requested_positions.find(header.key);
      if (requested == requested_positions.end()) {
        throw_node_error(_transport.target(), "get response contains an unrequested key");
      }
      if (!returned_keys.emplace(header.key).second) {
        throw_node_error(_transport.target(), "get response contains a duplicate key");
      }

      auto value = _transport.read_exact(header.value_size);
      if (_transport.read_exact(2) != "\r\n") {
        throw_node_error(_transport.target(), "value is not terminated by CRLF");
      }
      for (const auto position : requested->second) {
        values[position] = value;
      }
    }
  }

  // 执行 delete 删除命令
  DeleteStatus remove(std::string_view key) {
    _transport.begin_operation();
    _transport.write_all(make_key_request("delete", key));
    const auto response = _transport.read_line();

    if (response == "DELETED\r\n") {
      return DeleteStatus::Deleted;
    }
    if (response == "NOT_FOUND\r\n") {
      return DeleteStatus::NotFound;
    }

    throw_node_error(_transport.target(), "unexpected delete response " + quote(response));
  }

 private:
  TcpTransport _transport;
};

// 基于节点字符串规格构造集群客户端
ClusterClient::ClusterClient(std::string_view nodes, std::chrono::milliseconds timeout)
    : _ring{parse_nodes(nodes)}, _timeout{timeout} {
  validate_timeout(timeout);
}

// 基于节点列表构造集群客户端
ClusterClient::ClusterClient(const std::vector<Node>& nodes, std::chrono::milliseconds timeout)
    : _ring{nodes}, _timeout{timeout} {
  if (_ring.nodes().empty()) {
    throw std::invalid_argument("cluster client requires at least one node");
  }
  validate_timeout(timeout);
}

ClusterClient::~ClusterClient() = default;

// 根据 key 查询路由的责任节点
Node ClusterClient::route(std::string_view key) const { return _ring.route(key); }

// 获取或懒加载创建与指定节点的持久连接
ClusterClient::MemcachedConnection& ClusterClient::connection_for(const Node& node) {
  auto& connection = _connections[node.id()];
  if (!connection) {
    connection = std::make_unique<MemcachedConnection>(node, _timeout);
  }
  return *connection;
}

// 统一模板执行入口：复用目标节点的连接，并在异常时丢弃连接。
template <typename Operation>
auto ClusterClient::execute(const Node& node, Operation&& operation)
    -> decltype(operation(std::declval<MemcachedConnection&>())) {
  try {
    return std::forward<Operation>(operation)(connection_for(node));
  } catch (...) {
    // 出现异常时淘汰当前连接，下次请求将重新建立连接
    _connections.erase(node.id());
    throw;
  }
}

// 集群客户端对外 set API
bool ClusterClient::set(std::string_view key, std::string_view value) { return set(key, value, 0); }

bool ClusterClient::set(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) {
  constexpr std::uint32_t max_relative_ttl = 60U * 60U * 24U * 30U;
  if (ttl_seconds > max_relative_ttl) {
    throw std::invalid_argument{"relative TTL must not exceed 30 days"};
  }
  return execute(route(key), [&](MemcachedConnection& connection) {
    return connection.set(key, value, ttl_seconds);
  });
}

// 集群客户端对外 get API
std::optional<std::string> ClusterClient::get(std::string_view key) {
  return execute(route(key), [&](MemcachedConnection& connection) { return connection.get(key); });
}

std::vector<ClusterClient::NodeGetBatch> ClusterClient::group_get_keys(
    const std::vector<std::string>& keys) const {
  std::vector<NodeGetBatch> batches;
  std::unordered_map<std::string, std::size_t> node_positions;
  std::vector<std::unordered_map<std::string, std::size_t>> key_positions;

  for (std::size_t input_position = 0; input_position < keys.size(); ++input_position) {
    Node node = route(keys[input_position]);
    const std::string node_id = node.id();
    const auto [node_position, inserted_node] = node_positions.emplace(node_id, batches.size());
    if (inserted_node) {
      batches.push_back({std::move(node), {}, {}});
      key_positions.emplace_back();
    }

    auto& batch = batches[node_position->second];
    auto& node_key_positions = key_positions[node_position->second];
    const auto [key_position, inserted_key] =
        node_key_positions.emplace(keys[input_position], batch.keys.size());
    if (inserted_key) {
      batch.keys.push_back(keys[input_position]);
      batch.input_positions.push_back({input_position});
    } else {
      batch.input_positions[key_position->second].push_back(input_position);
    }
  }
  return batches;
}

std::vector<std::optional<std::string>> ClusterClient::get_many(
    const std::vector<std::string>& keys) {
  std::vector<std::optional<std::string>> values(keys.size());
  for (const auto& batch : group_get_keys(keys)) {
    const auto node_values = execute(batch.node, [&](MemcachedConnection& connection) {
      return connection.get_many(batch.keys);
    });
    for (std::size_t key_index = 0; key_index < batch.input_positions.size(); ++key_index) {
      for (const auto input_position : batch.input_positions[key_index]) {
        values[input_position] = node_values[key_index];
      }
    }
  }
  return values;
}

// 集群客户端对外 remove API
bool ClusterClient::remove(std::string_view key) {
  return remove_status(key) == DeleteStatus::Deleted;
}

// 集群客户端对外 remove_status API（返回具体 DeleteStatus 枚举）
DeleteStatus ClusterClient::remove_status(std::string_view key) {
  return execute(route(key),
                 [&](MemcachedConnection& connection) { return connection.remove(key); });
}

}  // namespace sphinx
