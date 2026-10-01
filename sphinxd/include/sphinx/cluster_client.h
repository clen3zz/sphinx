// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/cluster.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sphinx {

enum class DeleteStatus : std::uint8_t { Deleted, NotFound };

class ClientError : public std::runtime_error {
 public:
  explicit ClientError(const std::string& message) : std::runtime_error(message) {}
};

/// 为静态配置的缓存节点集合提供同步客户端。
class ClusterClient final {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout{2000};

  explicit ClusterClient(std::string_view nodes,
                         std::chrono::milliseconds timeout = kDefaultTimeout);
  explicit ClusterClient(const std::vector<Node>& nodes,
                         std::chrono::milliseconds timeout = kDefaultTimeout);
  ~ClusterClient();

  ClusterClient(const ClusterClient&) = delete;
  ClusterClient& operator=(const ClusterClient&) = delete;
  ClusterClient(ClusterClient&&) = delete;
  ClusterClient& operator=(ClusterClient&&) = delete;

  Node route(std::string_view key) const;

  bool set(std::string_view key, std::string_view value);

  /// Writes a value with a relative TTL in seconds (1..30 days). Zero retains legacy no-expiry
  /// behavior. Throws std::invalid_argument before I/O for a larger value. Not thread-safe.
  bool set(std::string_view key, std::string_view value, std::uint32_t ttl_seconds);

  std::optional<std::string> get(std::string_view key);

  /// Reads several keys while preserving input order and duplicate positions.
  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys);

  bool remove(std::string_view key);

  /// 返回协议层的删除结果。
  DeleteStatus remove_status(std::string_view key);

 private:
  class MemcachedConnection;

  struct NodeGetBatch {
    Node node;
    std::vector<std::string> keys;
    std::vector<std::vector<std::size_t>> input_positions;
  };

  MemcachedConnection& connection_for(const Node& node);
  std::vector<NodeGetBatch> group_get_keys(const std::vector<std::string>& keys) const;

  template <typename Operation>
  auto execute(const Node& node, Operation&& operation)
      -> decltype(operation(std::declval<MemcachedConnection&>()));

  ConsistentHashRing _ring;
  std::chrono::milliseconds _timeout;
  std::unordered_map<std::string, std::unique_ptr<MemcachedConnection>> _connections;
};

}  // namespace sphinx
