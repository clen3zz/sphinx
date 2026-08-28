// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/cluster.h>

#include <chrono>
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
  explicit ClientError(const std::string& message) : std::runtime_error(message) {
  }
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

  std::optional<std::string> get(std::string_view key);

  bool remove(std::string_view key);

  /// 返回协议层的删除结果。
  DeleteStatus remove_status(std::string_view key);

 private:
  class MemcachedConnection;

  MemcachedConnection& connection_for(const Node& node);

  template <typename Operation>
  auto execute(std::string_view key, Operation&& operation)
      -> decltype(operation(std::declval<MemcachedConnection&>()));

  ConsistentHashRing _ring;
  std::chrono::milliseconds _timeout;
  std::unordered_map<std::string, std::unique_ptr<MemcachedConnection>> _connections;
};

}  // namespace sphinx
