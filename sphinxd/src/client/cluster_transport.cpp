// SPDX-License-Identifier: Apache-2.0
#include "cluster_transport.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sphinx/cluster_client.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <system_error>
#include <utility>

#include "cluster_resolver.h"

namespace sphinx {
namespace {

constexpr size_t kMaxResponseLine = size_t{64} * 1024;
constexpr size_t kMaxValueBytes = size_t{8} * 1024 * 1024;

std::string errno_message(int err) { return std::generic_category().message(err); }

[[noreturn]] void throw_node_error(std::string_view target, std::string_view detail) {
  throw ClientError{std::string{"node "} + std::string{target} + ": " + std::string{detail}};
}

}  // namespace

// 底层同步非阻塞带超时机制的 TCP 传输通道
struct TcpTransport::Impl {
 public:
  Impl(const Node& node, std::chrono::milliseconds timeout)
      : _target{node.id()}, _host{node.host}, _port{node.port}, _timeout{timeout} {}

  ~Impl() { close(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  std::string_view target() const { return _target; }

  // One deadline covers connection setup, writing, and every response fragment.
  void begin_operation() { _deadline = std::chrono::steady_clock::now() + _timeout; }

  // 阻塞且带超时地发送全部消息字节
  void write_all(std::string_view message) {
    check_deadline();
    ensure_connected();
    size_t offset = 0;

    while (offset < message.size()) {
      wait_for(_fd, POLLOUT);

      const auto count = send(_fd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);

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
    while (true) {
      check_deadline();
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
    if (size > kMaxValueBytes) {
      throw_node_error(_target, "value length exceeds client limit");
    }
    std::string result;
    result.reserve(size);

    while (result.size() < size) {
      check_deadline();
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
    check_deadline();
    if (_fd >= 0) {
      return;
    }

    // 1. 解析目标节点主机名和端口
    const auto port = std::to_string(_port);
    const auto addresses = resolve_with_deadline(_host, port, _deadline, _target);
    check_deadline();

    // 2. 遍历解析到的地址依次尝试建立连接
    std::string last_error{"connection failed"};
    for (const auto* address = addresses.get(); address != nullptr; address = address->ai_next) {
      check_deadline();
      if (const auto fd = connect_to(address, &last_error); fd >= 0) {
        _fd = fd;
        return;
      }
    }

    throw_node_error(_target, last_error);
  }

  // 对指定目标地址执行非阻塞 connect 并结合 poll 等待三次握手完成
  int connect_to(const addrinfo* address, std::string* last_error) const {
    const auto fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
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
    const auto flags = fcntl(fd, F_GETFL, 0);
    const auto nonblocking_flags =
        static_cast<int>(static_cast<unsigned int>(flags) | static_cast<unsigned int>(O_NONBLOCK));
    if (flags < 0 || fcntl(fd, F_SETFL, nonblocking_flags) < 0) {
      return fail(errno);
    }

    // 发起连接
    const auto result = connect(fd, address->ai_addr, address->ai_addrlen);
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
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0) {
        return fail(error == 0 ? errno : error);
      }
    }

    return fd;
  }

  // 使用 poll 阻塞等待指定套接字事件触发，超时抛出异常
  void wait_for(int fd, short events) const {
    pollfd descriptor = {fd, events, 0};

    while (true) {
      const auto remaining = remaining_timeout();
      const auto timeout = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
      const auto result = poll(&descriptor, 1, static_cast<int>(timeout));

      if (result <= 0) {
        if (result == 0) {
          throw_node_error(_target, "operation timed out");
        }
        if (errno == EINTR) {
          continue;
        }
        throw_node_error(_target, errno_message(errno));
      }

      const auto ready_events = static_cast<unsigned int>(descriptor.revents);
      const auto requested_events = static_cast<unsigned int>(events) |
                                    static_cast<unsigned int>(POLLERR | POLLHUP | POLLNVAL);
      if ((ready_events & requested_events) != 0) {
        check_deadline();
        return;
      }
    }
  }

  std::chrono::steady_clock::duration remaining_timeout() const {
    const auto now = std::chrono::steady_clock::now();
    if (now >= _deadline) {
      throw_node_error(_target, "operation timed out");
    }
    return _deadline - now;
  }

  void check_deadline() const { (void)remaining_timeout(); }

  // 从套接字阻塞读入一批数据追加至 _read_buffer
  void read_some() {
    ensure_connected();
    wait_for(_fd, POLLIN);

    char buffer[16 * 1024];
    // resolve_with_deadline explicitly unlocks its resolver mutex before returning here.
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
    const auto count = recv(_fd, buffer, sizeof(buffer), 0);

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
  std::chrono::steady_clock::time_point _deadline{};
  std::string _read_buffer;
};

TcpTransport::TcpTransport(const Node& node, std::chrono::milliseconds timeout)
    : _impl{std::make_unique<Impl>(node, timeout)} {}

TcpTransport::~TcpTransport() = default;

std::string_view TcpTransport::target() const { return _impl->target(); }
void TcpTransport::begin_operation() { _impl->begin_operation(); }
void TcpTransport::write_all(std::string_view message) { _impl->write_all(message); }
std::string TcpTransport::read_line() { return _impl->read_line(); }
std::string TcpTransport::read_exact(size_t size) { return _impl->read_exact(size); }

}  // namespace sphinx
