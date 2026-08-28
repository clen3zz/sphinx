// SPDX-License-Identifier: Apache-2.0
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sphinx/cluster_client.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

namespace sphinx::cluster {
namespace {

constexpr size_t kMaxResponseLine = size_t{64} * 1024;

// 获取系统 errno 对应的可读错误信息
std::string errno_message(int err) {
  return std::generic_category().message(err);
}

// 抛出包含节点标识与具体细节的 ClientError 异常
[[noreturn]] void throw_node_error(std::string_view target, std::string_view detail) {
  throw ClientError{std::string{"node "} + std::string{target} + ": " + std::string{detail}};
}

// 为字符串添加单引号包裹
std::string quote(std::string_view value) {
  return "'" + std::string{value} + "'";
}

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
  if (timeout.count() <= 0) {
    throw std::invalid_argument("cluster client timeout must be positive");
  }
}

// 底层同步非阻塞带超时机制的 TCP 传输通道
class TcpTransport final {
 public:
  TcpTransport(const Node& node, std::chrono::milliseconds timeout)
      : _target{node.id()}, _host{node.host}, _port{node.port}, _timeout{timeout} {
  }

  ~TcpTransport() {
    close();
  }

  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  std::string_view target() const {
    return _target;
  }

  // 阻塞且带超时地发送全部消息字节
  void write_all(std::string_view message) {
    ensure_connected();
    size_t offset = 0;

    while (offset < message.size()) {
      wait_for(_fd, POLLOUT);

      const auto count =
          ::send(_fd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);

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

      throw_node_error(_target, errno_message(errno));
    }
  }

  // 从接收流中读取单行以 \r\n 结尾的协议文本
  std::string read_line() {
    for (;;) {
      // 1. 尝试在当前接收缓冲区中寻找行终结符
      if (const auto separator = _read_buffer.find("\r\n"); separator != std::string::npos) {
        const auto line_end = separator + 2;
        auto line = _read_buffer.substr(0, line_end);
        _read_buffer.erase(0, line_end);
        return line;
      }

      // 2. 防御超长响应行攻击
      if (_read_buffer.size() > kMaxResponseLine) {
        throw_node_error(_target, "response line is too long");
      }

      // 3. 阻塞读取更多数据填入缓冲区
      read_some();
    }
  }

  // 从接收流中精准读取指定字节数的数据块
  std::string read_exact(size_t size) {
    std::string result;
    result.reserve(size);

    while (result.size() < size) {
      if (_read_buffer.empty()) {
        read_some();
        continue;
      }

      const auto count = std::min(size - result.size(), _read_buffer.size());
      result.append(_read_buffer.data(), count);
      _read_buffer.erase(0, count);
    }

    return result;
  }

 private:
  // 确保 TCP 连接已建立
  void ensure_connected() {
    if (_fd >= 0) {
      return;
    }

    // 1. 解析目标节点主机名和端口
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const auto port = std::to_string(_port);
    addrinfo* addresses = nullptr;

    const auto status = ::getaddrinfo(_host.c_str(), port.c_str(), &hints, &addresses);
    if (status != 0) {
      throw_node_error(_target, std::string{"cannot resolve host: "} + ::gai_strerror(status));
    }

    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> address_guard{addresses, &::freeaddrinfo};

    // 2. 遍历解析到的地址依次尝试建立连接
    std::string last_error{"connection failed"};
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
      if (const auto fd = connect_to(address, &last_error); fd >= 0) {
        _fd = fd;
        return;
      }
    }

    throw_node_error(_target, last_error);
  }

  // 对指定目标地址执行非阻塞 connect 并结合 poll 等待三次握手完成
  int connect_to(const addrinfo* address, std::string* last_error) {
    const auto fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      *last_error = errno_message(errno);
      return -1;
    }

    const auto fail = [&](int error) {
      *last_error = errno_message(error);
      ::close(fd);
      return -1;
    };

    // 设置非阻塞
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      return fail(errno);
    }

    // 发起连接
    const auto result = ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (result < 0 && errno != EINPROGRESS) {
      return fail(errno);
    }

    // 若未立刻完成，poll 等待写就绪并检查套接字连接错误码
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
        return fail(error == 0 ? errno : error);
      }
    }

    return fd;
  }

  // 使用 poll 阻塞等待指定套接字事件触发，超时抛出异常
  void wait_for(int fd, short events) const {
    pollfd descriptor = {fd, events, 0};
    const auto timeout =
        std::clamp<int64_t>(_timeout.count(), int64_t{1}, std::numeric_limits<int>::max());

    for (;;) {
      const auto result = ::poll(&descriptor, 1, static_cast<int>(timeout));

      if (result <= 0) {
        if (result == 0) {
          throw_node_error(_target, "operation timed out");
        }
        if (errno == EINTR) {
          continue;
        }
        throw_node_error(_target, errno_message(errno));
      }

      if ((descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return;
      }
    }
  }

  // 从套接字阻塞读入一批数据追加至 _read_buffer
  void read_some() {
    ensure_connected();
    wait_for(_fd, POLLIN);

    char buffer[16 * 1024];
    const auto count = ::recv(_fd, buffer, sizeof(buffer), 0);

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

    throw_node_error(_target, errno_message(errno));
  }

  // 关闭套接字并清空接收缓冲
  void close() {
    if (_fd >= 0) {
      ::close(_fd);
    }

    _fd = -1;
    _read_buffer.clear();
  }

  int _fd = -1;
  std::string _target;
  std::string _host;
  uint16_t _port;
  std::chrono::milliseconds _timeout;
  std::string _read_buffer;
};

// 解析 get 响应行头部：VALUE <key> <flags> <bytes>\r\n，返回载荷字节长度
size_t parse_value_header(std::string_view target, const std::string& response,
                          std::string_view key) {
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

  // 3. 校验返回的 key 是否与请求一致
  if (body.substr(0, first_space) != key) {
    throw_node_error(target, "get response key does not match request");
  }

  // 4. 解析 flags 字段
  const auto flags =
      parse_decimal(target, "flags", body.substr(first_space + 1, second_space - first_space - 1));
  if (flags > std::numeric_limits<uint32_t>::max()) {
    throw_node_error(target, "flags are out of range in response");
  }

  // 5. 解析载荷字节大小字段
  const auto length = body.substr(second_space + 1);
  if (length.empty()) {
    throw_node_error(target, "missing value length in get response");
  }

  const auto bytes = parse_decimal(target, "value length", length);
  if (bytes > std::numeric_limits<size_t>::max()) {
    throw_node_error(target, "value length is too large");
  }

  return static_cast<size_t>(bytes);
}

}  // namespace

// 单个 Memcached 节点连接客户端封装
class ClusterClient::MemcachedConnection final {
 public:
  MemcachedConnection(const Node& node, std::chrono::milliseconds timeout)
      : _transport{node, timeout} {
  }

  MemcachedConnection(const MemcachedConnection&) = delete;
  MemcachedConnection& operator=(const MemcachedConnection&) = delete;

  // 执行 set 写入命令
  bool set(std::string_view key, std::string_view value) {
    // 1. 构建标准 set 请求帧
    std::string request{"set "};
    request.reserve(32 + key.size() + value.size());
    request += key;
    request += " 0 0 ";
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

  // 执行 get 查询命令
  std::optional<std::string> get(std::string_view key) {
    // 1. 发送 get 请求
    _transport.write_all(make_key_request("get", key));

    // 2. 读取第一行头部响应
    const auto header = _transport.read_line();
    if (header == "END\r\n") {
      return std::nullopt;
    }

    // 3. 解析 VALUE 头部并读取精准长度的正文载荷
    const auto value_size = parse_value_header(_transport.target(), header, key);
    auto value = _transport.read_exact(value_size);

    // 4. 校验载荷末尾 CRLF 以及随后的 END\r\n 终结符
    if (_transport.read_exact(2) != "\r\n") {
      throw_node_error(_transport.target(), "value is not terminated by CRLF");
    }
    if (_transport.read_line() != "END\r\n") {
      throw_node_error(_transport.target(), "get response is not terminated by END");
    }

    return value;
  }

  // 执行 delete 删除命令
  DeleteStatus remove(std::string_view key) {
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
Node ClusterClient::route(std::string_view key) const {
  return _ring.route(key);
}

// 获取或懒加载创建与指定节点的持久连接
ClusterClient::MemcachedConnection& ClusterClient::connection_for(const Node& node) {
  auto& connection = _connections[node.id()];
  if (!connection) {
    connection = std::make_unique<MemcachedConnection>(node, _timeout);
  }
  return *connection;
}

// 统一模板执行入口：完成 key 路由、连接复用以及异常时连接重置
template <typename Operation>
auto ClusterClient::execute(std::string_view key, Operation&& operation)
    -> decltype(operation(std::declval<MemcachedConnection&>())) {
  const auto node = route(key);

  try {
    return std::forward<Operation>(operation)(connection_for(node));
  } catch (...) {
    // 出现异常时淘汰当前连接，下次请求将重新建立连接
    _connections.erase(node.id());
    throw;
  }
}

// 集群客户端对外 set API
bool ClusterClient::set(std::string_view key, std::string_view value) {
  return execute(key, [&](MemcachedConnection& connection) { return connection.set(key, value); });
}

// 集群客户端对外 get API
std::optional<std::string> ClusterClient::get(std::string_view key) {
  return execute(key, [&](MemcachedConnection& connection) { return connection.get(key); });
}

// 集群客户端对外 remove API
bool ClusterClient::remove(std::string_view key) {
  return remove_status(key) == DeleteStatus::Deleted;
}

// 集群客户端对外 remove_status API（返回具体 DeleteStatus 枚举）
DeleteStatus ClusterClient::remove_status(std::string_view key) {
  return execute(key, [&](MemcachedConnection& connection) { return connection.remove(key); });
}

}  // namespace sphinx::cluster
