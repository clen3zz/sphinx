#include <sphinx/cluster_client.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <limits>
#include <memory>

namespace sphinx::cluster {
namespace {

std::string
node_id(const Node& node)
{
  return node.id();
}

int
poll_timeout(std::chrono::milliseconds timeout)
{
  if (timeout.count() <= 0) {
    return 1;
  }
  auto count = timeout.count();
  if (count > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(count);
}

[[noreturn]] void
throw_node_error(const std::string& target, const std::string& detail)
{
  throw ClientError("node " + target + ": " + detail);
}

uint64_t
parse_decimal(std::string_view value, const std::string& target, const char* field)
{
  if (value.empty()) {
    throw_node_error(target, std::string("invalid ") + field + " in response");
  }
  uint64_t result = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      throw_node_error(target, std::string("invalid ") + field + " in response");
    }
    auto digit = static_cast<uint64_t>(ch - '0');
    if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      throw_node_error(target, std::string("invalid ") + field + " in response");
    }
    result = result * 10 + digit;
  }
  return result;
}

} // namespace

class ClusterClient::Connection final
{
public:
  Connection(const Node& node, std::chrono::milliseconds timeout)
    : _target{node_id(node)}
    , _host{node.host}
    , _port{node.port}
    , _timeout{timeout}
  {
  }

  ~Connection()
  {
    close();
  }

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void set(std::string_view key, std::string_view value)
  {
    std::string request;
    request.reserve(32 + key.size() + value.size());
    request.append("set ");
    request.append(key);
    request.append(" 0 0 ");
    request.append(std::to_string(value.size()));
    request.append("\r\n");
    request.append(value);
    request.append("\r\n");
    write_all(request);
    auto response = read_line();
    if (response == "STORED\r\n") {
      return;
    }
    throw_node_error(_target, "unexpected set response " + quote(response));
  }

  std::optional<std::string> get(std::string_view key)
  {
    std::string request;
    request.reserve(6 + key.size());
    request.append("get ");
    request.append(key);
    request.append("\r\n");
    write_all(request);

    auto header = read_line();
    if (header == "END\r\n") {
      return std::nullopt;
    }
    if (header.size() < 10 || header.compare(0, 6, "VALUE ") != 0) {
      throw_node_error(_target, "unexpected get response " + quote(header));
    }

    auto body = std::string_view{header}.substr(6, header.size() - 8);
    auto first_space = body.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
      throw_node_error(_target, "malformed get header " + quote(header));
    }
    auto response_key = body.substr(0, first_space);
    if (response_key != key) {
      throw_node_error(_target, "get response key does not match request");
    }
    body.remove_prefix(first_space + 1);
    auto second_space = body.find(' ');
    if (second_space == std::string_view::npos || second_space == 0) {
      throw_node_error(_target, "malformed get header " + quote(header));
    }
    const auto flags = parse_decimal(body.substr(0, second_space), _target, "flags");
    if (flags > std::numeric_limits<uint32_t>::max()) {
      throw_node_error(_target, "flags are out of range in response");
    }
    body.remove_prefix(second_space + 1);
    if (body.empty()) {
      throw_node_error(_target, "missing value length in get response");
    }
    auto bytes = parse_decimal(body, _target, "value length");
    if (bytes > std::numeric_limits<size_t>::max()) {
      throw_node_error(_target, "value length is too large");
    }

    auto value = read_exact(static_cast<size_t>(bytes));
    if (read_exact(2) != "\r\n") {
      throw_node_error(_target, "value is not terminated by CRLF");
    }
    if (read_line() != "END\r\n") {
      throw_node_error(_target, "get response is not terminated by END");
    }
    return value;
  }

  DeleteStatus remove(std::string_view key)
  {
    std::string request;
    request.reserve(9 + key.size());
    request.append("delete ");
    request.append(key);
    request.append("\r\n");
    write_all(request);
    auto response = read_line();
    if (response == "DELETED\r\n") {
      return DeleteStatus::Deleted;
    }
    if (response == "NOT_FOUND\r\n") {
      return DeleteStatus::NotFound;
    }
    throw_node_error(_target, "unexpected delete response " + quote(response));
  }

private:
  static std::string quote(std::string_view value)
  {
    std::string result{"'"};
    result.append(value);
    result.push_back('\'');
    return result;
  }

  void ensure_connected()
  {
    if (_fd >= 0) {
      return;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    auto port = std::to_string(_port);
    struct addrinfo* addresses = nullptr;
    auto status = ::getaddrinfo(_host.c_str(), port.c_str(), &hints, &addresses);
    if (status != 0) {
      throw_node_error(_target, std::string{"cannot resolve host: "} + ::gai_strerror(status));
    }
    std::unique_ptr<struct addrinfo, decltype(&::freeaddrinfo)> address_guard{addresses,
                                                                              &::freeaddrinfo};

    int fd = -1;
    std::string last_error{"connection failed"};
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
      fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd < 0) {
        last_error = std::strerror(errno);
        continue;
      }
      auto flags = ::fcntl(fd, F_GETFL, 0);
      if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        last_error = std::strerror(errno);
        ::close(fd);
        fd = -1;
        continue;
      }
      auto result = ::connect(fd, address->ai_addr, address->ai_addrlen);
      if (result < 0 && errno != EINPROGRESS) {
        last_error = std::strerror(errno);
        ::close(fd);
        fd = -1;
        continue;
      }
      if (result < 0) {
        try {
          wait_for(fd, POLLOUT);
        } catch (...) {
          ::close(fd);
          throw;
        }
        int error = 0;
        socklen_t error_size = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0) {
          last_error = error == 0 ? std::strerror(errno) : std::strerror(error);
          ::close(fd);
          fd = -1;
          continue;
        }
      }
      break;
    }
    if (fd < 0) {
      throw_node_error(_target, last_error);
    }
    _fd = fd;
  }

  void wait_for(int fd, short events)
  {
    struct pollfd descriptor = {fd, events, 0};
    for (;;) {
      auto result = ::poll(&descriptor, 1, poll_timeout(_timeout));
      if (result == 0) {
        throw_node_error(_target, "operation timed out");
      }
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_node_error(_target, std::strerror(errno));
      }
      if ((descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return;
      }
    }
  }

  void write_all(std::string_view message)
  {
    ensure_connected();
    size_t offset = 0;
    while (offset < message.size()) {
      wait_for(_fd, POLLOUT);
      auto count = ::send(_fd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
      if (count > 0) {
        offset += static_cast<size_t>(count);
        continue;
      }
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (count == 0) {
        throw_node_error(_target, "connection closed while writing");
      }
      throw_node_error(_target, std::strerror(errno));
    }
  }

  std::string read_line()
  {
    for (;;) {
      auto separator = _read_buffer.find("\r\n");
      if (separator != std::string::npos) {
        auto line_end = separator + 2;
        auto line = _read_buffer.substr(0, line_end);
        _read_buffer.erase(0, line_end);
        return line;
      }
      if (_read_buffer.size() > 64 * 1024) {
        throw_node_error(_target, "response line is too long");
      }
      read_some();
    }
  }

  std::string read_exact(size_t size)
  {
    std::string result;
    result.reserve(size);
    while (result.size() < size) {
      if (!_read_buffer.empty()) {
        auto count = std::min(size - result.size(), _read_buffer.size());
        result.append(_read_buffer.data(), count);
        _read_buffer.erase(0, count);
        continue;
      }
      read_some();
    }
    return result;
  }

  void read_some()
  {
    ensure_connected();
    wait_for(_fd, POLLIN);
    char buffer[16 * 1024];
    auto count = ::recv(_fd, buffer, sizeof(buffer), 0);
    if (count > 0) {
      _read_buffer.append(buffer, static_cast<size_t>(count));
      return;
    }
    if (count == 0) {
      throw_node_error(_target, "connection closed while reading");
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    throw_node_error(_target, std::strerror(errno));
  }

  void close()
  {
    if (_fd >= 0) {
      ::close(_fd);
      _fd = -1;
    }
    _read_buffer.clear();
  }

  int _fd = -1;
  std::string _target;
  std::string _host;
  uint16_t _port;
  std::chrono::milliseconds _timeout;
  std::string _read_buffer;
};

ClusterClient::ClusterClient(std::string_view nodes, std::chrono::milliseconds timeout)
  : _nodes{parse_nodes(nodes)}
  , _ring{_nodes}
  , _timeout{timeout}
{
  if (timeout.count() <= 0) {
    throw std::invalid_argument("cluster client timeout must be positive");
  }
}

ClusterClient::ClusterClient(const std::vector<Node>& nodes, std::chrono::milliseconds timeout)
  : _nodes{nodes}
  , _ring{_nodes}
  , _timeout{timeout}
{
  if (_nodes.empty()) {
    throw std::invalid_argument("cluster client requires at least one node");
  }
  if (timeout.count() <= 0) {
    throw std::invalid_argument("cluster client timeout must be positive");
  }
}

ClusterClient::~ClusterClient() = default;

Node
ClusterClient::route(std::string_view key) const
{
  return _ring.route(key);
}

std::shared_ptr<ClusterClient::Connection>&
ClusterClient::connection_for(const Node& node)
{
  auto key = node_id(node);
  auto& connection = _connections[key];
  if (!connection) {
    connection = std::make_shared<Connection>(node, _timeout);
  }
  return connection;
}

void
ClusterClient::close_connection(const Node& node)
{
  _connections.erase(node_id(node));
}

bool
ClusterClient::set(std::string_view key, std::string_view value)
{
  const auto& node = route(key);
  try {
    connection_for(node)->set(key, value);
    return true;
  } catch (...) {
    close_connection(node);
    throw;
  }
}

std::optional<std::string>
ClusterClient::get(std::string_view key)
{
  const auto& node = route(key);
  try {
    return connection_for(node)->get(key);
  } catch (...) {
    close_connection(node);
    throw;
  }
}

bool
ClusterClient::remove(std::string_view key)
{
  return remove_status(key) == DeleteStatus::Deleted;
}

DeleteStatus
ClusterClient::remove_status(std::string_view key)
{
  const auto& node = route(key);
  try {
    return connection_for(node)->remove(key);
  } catch (...) {
    close_connection(node);
    throw;
  }
}

} // namespace sphinx::cluster
