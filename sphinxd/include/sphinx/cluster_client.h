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
#include <vector>

namespace sphinx::cluster {

/// The result of deleting a key on its owner node.
enum class DeleteStatus
{
  Deleted,
  NotFound,
};

/// Error raised for connection, timeout, or malformed Memcached responses.
class ClientError : public std::runtime_error
{
public:
  explicit ClientError(const std::string& message)
    : std::runtime_error(message)
  {
  }
};

/// A synchronous client for a statically configured set of cache nodes.
///
/// The client is deliberately not thread-safe.  Each instance owns at most
/// one lazily-created connection per node and reuses it for later operations.
class ClusterClient final
{
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

  /// Return the statically selected owner for key.
  Node route(std::string_view key) const;

  /// Store key/value with flags=0 and exptime=0.
  bool set(std::string_view key, std::string_view value);

  /// Return a hit, std::nullopt for a cache miss, or throw ClientError.
  std::optional<std::string> get(std::string_view key);

  /// Delete key, returning whether a value was deleted.  A missing key is not
  /// an error; network and protocol failures throw ClientError.
  bool remove(std::string_view key);

  /// Return the full delete status when callers need to print the protocol
  /// outcome rather than just test whether deletion happened.
  DeleteStatus remove_status(std::string_view key);

private:
  class Connection;

  std::shared_ptr<Connection>& connection_for(const Node& node);
  void close_connection(const Node& node);

  std::vector<Node> _nodes;
  ConsistentHashRing _ring;
  std::chrono::milliseconds _timeout;
  std::unordered_map<std::string, std::shared_ptr<Connection>> _connections;
};

} // namespace sphinx::cluster
