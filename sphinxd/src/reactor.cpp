// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <netdb.h>
#include <netinet/tcp.h>
#include <sphinx/reactor-epoll.h>
#include <sphinx/reactor.h>
#include <sphinx/spsc_queue.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>

namespace sphinx::reactor {

// Socket 基础抽象类构造函数
Socket::Socket(int sockfd) : _sockfd{sockfd} {
}

// 析构函数：RAII 自动关闭底层套接字文件描述符
Socket::~Socket() {
  ::close(_sockfd);
}

// 获取底层文件描述符
int Socket::fd() const {
  return _sockfd;
}

// TcpListener 构造函数：接管监听套接字并绑定新连接回调函数
TcpListener::TcpListener(int sockfd, TcpAcceptFn&& accept_fn)
    : _sockfd{sockfd}, _accept_fn{accept_fn} {
}

// 析构函数：关闭监听套接字
TcpListener::~TcpListener() {
  ::close(_sockfd);
}

// 监听套接字读事件就绪回调：循环接收所有就绪的新客户端连接
void TcpListener::on_pollin() {
  accept();
}

// 监听套接字无需关注写就绪
bool TcpListener::on_pollout() {
  return true;
}

// 非阻塞循环 accept 所有挂起的新连接
void TcpListener::accept() {
  for (;;) {
    // 采用 accept4 原子创建非阻塞与进程退出自动关闭的连接套接字
    int connfd = ::accept4(_sockfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (connfd >= 0) {
      try {
        _accept_fn(connfd);
      } catch (...) {
        ::close(connfd);
        throw;
      }
      continue;
    }

    // 信号中断则立即重试
    if (errno == EINTR) {
      continue;
    }

    // 队列已无更多就绪连接，退出循环
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    throw std::system_error(errno, std::system_category(), "accept4");
  }
}

// 获取监听套接字描述符
int TcpListener::fd() const {
  return _sockfd;
}

// 辅助函数：根据网卡地址与端口解析网络地址结构体列表
static addrinfo* lookup_addresses(const std::string& iface, int port, int sock_type) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = sock_type;
  hints.ai_protocol = 0;
  hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;

  addrinfo* ret = nullptr;
  int err = getaddrinfo(iface.c_str(), std::to_string(port).c_str(), &hints, &ret);
  if (err != 0) {
    throw std::runtime_error("'" + iface + "': " + gai_strerror(err));
  }

  return ret;
}

// 创建并初始化 TCP 监听器（配置端口复用、绑定地址并开启监听）
std::shared_ptr<TcpListener> make_tcp_listener(const std::string& iface, int port, int backlog,
                                               TcpAcceptFn&& accept_fn) {
  auto* addresses = lookup_addresses(iface, port, SOCK_STREAM);

  for (addrinfo* rp = addresses; rp != nullptr; rp = rp->ai_next) {
    // 1. 创建非阻塞 TCP 套接字
    int sockfd =
        ::socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, rp->ai_protocol);
    if (sockfd < 0) {
      continue;
    }

    // 2. 配置 SO_REUSEADDR 与 SO_REUSEPORT（支持多线程独立绑定同端口实现内核级负载均衡）
    int one = 1;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    // 3. 绑定目标地址与端口
    if (::bind(sockfd, rp->ai_addr, rp->ai_addrlen) < 0) {
      ::close(sockfd);
      continue;
    }

    // 4. 开启监听
    if (::listen(sockfd, backlog) < 0) {
      ::close(sockfd);
      continue;
    }

    freeaddrinfo(addresses);

    try {
      return std::make_shared<TcpListener>(sockfd, std::move(accept_fn));
    } catch (...) {
      ::close(sockfd);
      throw;
    }
  }

  freeaddrinfo(addresses);
  throw std::runtime_error("Failed to listen to interface: '" + iface + "'");
}

// TcpSocket 构造函数
TcpSocket::TcpSocket(int sockfd, TcpRecvFn&& recv_fn) : Socket{sockfd}, _recv_fn{recv_fn} {
}

TcpSocket::~TcpSocket() = default;

// 设置 TCP_NODELAY 禁用 Nagle 算法，降低网络长尾延迟
void TcpSocket::set_tcp_nodelay(bool nodelay) {
  int value = nodelay ? 1 : 0;
  if (setsockopt(_sockfd, SOL_TCP, TCP_NODELAY, &value, sizeof(value)) < 0) {
    throw std::system_error(errno, std::system_category(), "setsockopt");
  }
}

// 检查套接字是否已断开关闭
bool TcpSocket::closed() const {
  return _closed;
}

// 同步尝试发送数据；若无法立即全部发送则缓存至发送队列并返回 false（需 Reactor 异步写）
bool TcpSocket::send(const char* msg, size_t len) {
  if (_closed) {
    return true;
  }
  if (len == 0) {
    return true;
  }

  // 若发送队列中已有积压数据，必须按序先排队后发送
  if (!_tx_buf.empty()) {
    _tx_buf.insert(_tx_buf.end(), msg, msg + len);
    return false;
  }

  // 尝试非阻塞系统调用发送
  ssize_t nr;
  do {
    nr = ::send(_sockfd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (nr < 0 && errno == EINTR);

  // 对端异常重置或套接字无效
  if (nr < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN || errno == EBADF)) {
    _closed = true;
    return true;
  }

  // 内核发送缓冲区已满
  if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    _tx_buf.insert(_tx_buf.end(), msg, msg + len);
    return false;
  }

  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "send");
  }

  // 部分写入，将剩余数据缓存至发送缓冲区
  if (nr == 0 || static_cast<size_t>(nr) < len) {
    _tx_buf.insert(_tx_buf.end(), msg + nr, msg + len);
    return false;
  }

  return true;
}

// 读事件就绪回调：读取数据并通知上层接收回调
void TcpSocket::on_pollin() {
  constexpr size_t rx_buf_size = size_t{256} * 1024;
  std::array<char, rx_buf_size> rx_buf;

  for (;;) {
    ssize_t nr = ::recv(_sockfd, rx_buf.data(), rx_buf.size(), MSG_DONTWAIT);

    // 成功读取到有效数据
    if (nr > 0) {
      _recv_fn(this->shared_from_this(),
               std::string_view{rx_buf.data(), static_cast<std::string_view::size_type>(nr)});
      return;
    }

    // 读到 EOF 说明对端已正常关闭连接
    if (nr == 0) {
      _closed = true;
      _recv_fn(this->shared_from_this(), std::string_view{});
      return;
    }

    // 信号中断重试
    if (errno == EINTR) {
      continue;
    }

    // 缓冲区已读空
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    // 连接发生异常重置或失效
    if (errno == ECONNRESET || errno == ENOTCONN || errno == EBADF) {
      _closed = true;
      _recv_fn(this->shared_from_this(), std::string_view{});
      return;
    }

    throw std::system_error(errno, std::system_category(), "recv");
  }
}

// 写事件就绪回调：继续发送未写完的缓冲区数据
bool TcpSocket::on_pollout() {
  if (_closed || _tx_buf.empty()) {
    return true;
  }

  ssize_t nr;
  do {
    nr = ::send(_sockfd, _tx_buf.data(), _tx_buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (nr < 0 && errno == EINTR);

  if (nr < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN || errno == EBADF)) {
    _closed = true;
    return true;
  }

  if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return false;
  }

  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "send");
  }

  if (nr == 0) {
    return false;
  }

  // 擦除已成功写入的部分
  _tx_buf.erase(_tx_buf.begin(), _tx_buf.begin() + nr);
  return _tx_buf.empty();
}

// 跨线程单向消息通道：无锁 SPSC 环形队列配合溢出链表队列
struct ReactorGroup::Channel {
  sphinx::spsc::Queue<MessagePtr, reactor_message_queue_size> queue;
  std::mutex overflow_mutex;
  std::deque<MessagePtr> overflow;
};

// 校验线程数合法性
static size_t checked_thread_count(size_t nr_threads) {
  if (nr_threads == 0 || nr_threads > max_nr_threads) {
    throw std::invalid_argument("invalid reactor thread count");
  }
  return nr_threads;
}

// ReactorGroup 构造函数：为所有线程初始化休眠状态位与跨线程唤醒 eventfd
ReactorGroup::ReactorGroup(size_t nr_threads)
    : _nr_threads{checked_thread_count(nr_threads)},
      _eventfds(_nr_threads, -1),
      _thread_is_sleeping(_nr_threads),
      _channels(_nr_threads * _nr_threads) {
  for (size_t id = 0; id < nr_threads; id++) {
    _thread_is_sleeping[id].store(false, std::memory_order_relaxed);
    _eventfds[id] = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (_eventfds[id] < 0) {
      auto saved_errno = errno;
      for (size_t close_id = 0; close_id < id; close_id++) {
        ::close(_eventfds[close_id]);
        _eventfds[close_id] = -1;
      }
      throw std::system_error(saved_errno, std::system_category(), "eventfd");
    }
  }
}

// 析构函数：关闭所有分配的 eventfd 描述符
ReactorGroup::~ReactorGroup() {
  for (auto& efd : _eventfds) {
    if (efd >= 0) {
      ::close(efd);
      efd = -1;
    }
  }
}

size_t ReactorGroup::nr_threads() const noexcept {
  return _nr_threads;
}

// 获取从源线程到目标线程的单向通信通道
ReactorGroup::Channel& ReactorGroup::channel(size_t destination, size_t source) {
  if (destination >= _nr_threads || source >= _nr_threads) {
    throw std::invalid_argument("invalid reactor message target");
  }

  auto& slot = _channels[destination * _nr_threads + source];
  if (!slot) {
    throw std::logic_error("reactor channel is not initialized");
  }

  return *slot;
}

// 为指定线程初始化其与其他所有对等线程间的双向通信通道
void ReactorGroup::initialize_thread(size_t thread_id) {
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor thread id");
  }

  std::scoped_lock lock{_channels_mutex};
  for (size_t peer = 0; peer < _nr_threads; peer++) {
    if (peer == thread_id) {
      continue;
    }

    auto& outgoing = _channels[peer * _nr_threads + thread_id];
    if (!outgoing) {
      outgoing = std::make_unique<Channel>();
    }

    auto& incoming = _channels[thread_id * _nr_threads + peer];
    if (!incoming) {
      incoming = std::make_unique<Channel>();
    }
  }
}

// 获取指定线程关联的 eventfd 描述符
int ReactorGroup::eventfd(size_t thread_id) const {
  if (thread_id >= _nr_threads || _eventfds[thread_id] < 0) {
    throw std::invalid_argument("invalid reactor wakeup target");
  }
  return _eventfds[thread_id];
}

// 查询指定线程当前是否处于睡眠等待状态
bool ReactorGroup::is_thread_sleeping(size_t thread_id) const {
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor wakeup target");
  }
  return _thread_is_sleeping[thread_id].load(std::memory_order_seq_cst);
}

// 设置指定线程的睡眠状态标记
void ReactorGroup::set_thread_sleeping(size_t thread_id, bool sleeping) {
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor wakeup target");
  }
  _thread_is_sleeping[thread_id].store(sleeping, std::memory_order_seq_cst);
}

// 默认 I/O 多路复用后端
std::string Reactor::default_backend() {
  return "epoll";
}

// Reactor 基类构造函数
Reactor::Reactor(size_t thread_id, std::shared_ptr<ReactorGroup> group, OnMessageFn&& on_message_fn)
    : _group{std::move(group)},
      _thread_id{thread_id},
      _nr_threads{0},
      _on_message_fn{std::move(on_message_fn)} {
  if (!_group) {
    throw std::invalid_argument("reactor group cannot be null");
  }

  _nr_threads = _group->nr_threads();
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor thread id");
  }

  _group->initialize_thread(_thread_id);
  _efd = _group->eventfd(_thread_id);
}

size_t Reactor::thread_id() const {
  return _thread_id;
}

size_t Reactor::nr_threads() const {
  return _nr_threads;
}

// 向目标工作线程发送跨线程消息（有界无锁队列满时直接返回失败）
bool Reactor::send_msg(size_t remote_id, const MessagePtr& message) {
  return send_msg_impl(remote_id, message, false);
}

// 向目标工作线程发送跨线程消息（允许在有界队列满时排入延迟溢出队列）
bool Reactor::send_msg_deferred(size_t remote_id, const MessagePtr& message) {
  return send_msg_impl(remote_id, message, true);
}

// 跨线程消息发送具体实现
bool Reactor::send_msg_impl(size_t remote_id, const MessagePtr& message, bool defer_if_full) {
  if (remote_id == _thread_id) {
    throw std::invalid_argument("Attempting to send message to self");
  }
  if (remote_id >= _nr_threads || !message) {
    throw std::invalid_argument("invalid reactor message target");
  }

  auto& channel = _group->channel(remote_id, _thread_id);

  {
    std::scoped_lock lock{channel.overflow_mutex};

    // 一旦出现溢出消息，后续消息也放入溢出队列，确保目的端观察到的顺序与队列保持严格一致
    if (channel.overflow.empty() && channel.queue.try_to_emplace(message)) {
      // 有界无锁环形队列已成功接纳该消息
    } else if (defer_if_full) {
      try {
        channel.overflow.emplace_back(message);
      } catch (const std::bad_alloc&) {
        return false;
      }
    } else {
      return false;
    }
  }

  // 标记对端线程待唤醒
  _pending_wakeups.set(remote_id);
  return true;
}

// 批量唤醒所有有待处理消息且当前正在休眠的对端线程
void Reactor::wake_up_pending() {
  for (size_t id = 0; id < _pending_wakeups.size(); id++) {
    if (_pending_wakeups.test(id) && _group->is_thread_sleeping(id)) {
      _group->set_thread_sleeping(id, false);
      wake_up(id);
    }
  }

  _pending_wakeups.reset();
}

// 向目标线程的 eventfd 写入数据以触发其 epoll_wait 唤醒
void Reactor::wake_up(size_t thread_id) {
  auto efd = _group->eventfd(thread_id);
  if (::eventfd_write(efd, 1) < 0) {
    if (errno == EAGAIN) {
      return;
    }
    throw std::system_error(errno, std::system_category(), "eventfd_write");
  }
}

// 探测当前线程的所有入站通道是否含有未消费消息
bool Reactor::has_messages() {
  for (size_t other = 0; other < _nr_threads; other++) {
    if (other == _thread_id) {
      continue;
    }

    auto& channel = _group->channel(_thread_id, other);
    if (channel.queue.front() != nullptr) {
      return true;
    }

    std::scoped_lock lock{channel.overflow_mutex};
    if (!channel.overflow.empty()) {
      return true;
    }
  }

  return false;
}

// 轮询并分发当前线程收到的所有跨线程消息
bool Reactor::poll_messages() {
  bool received = false;

  for (size_t other = 0; other < _nr_threads; other++) {
    if (other == _thread_id) {
      continue;
    }

    auto& channel = _group->channel(_thread_id, other);

    // 1. 优先消费无锁 SPSC 环形队列中的消息
    for (;;) {
      auto* queued = channel.queue.front();
      if (!queued) {
        break;
      }

      MessagePtr message = std::move(*queued);
      channel.queue.pop();
      received = true;
      _on_message_fn(std::move(message));
    }

    // 2. 消费溢出队列中的积压消息
    for (;;) {
      MessagePtr message;
      {
        std::scoped_lock lock{channel.overflow_mutex};
        if (channel.overflow.empty()) {
          break;
        }

        message = std::move(channel.overflow.front());
        channel.overflow.pop_front();
      }

      received = true;
      _on_message_fn(std::move(message));
    }
  }

  return received;
}

// Reactor 实例工厂方法
std::unique_ptr<Reactor> make_reactor(const std::string& backend, size_t thread_id,
                                      std::shared_ptr<ReactorGroup> group,
                                      OnMessageFn&& on_message_fn) {
  if (backend == "epoll") {
    return std::make_unique<EpollReactor>(thread_id, std::move(group), std::move(on_message_fn));
  }

  throw std::invalid_argument("unrecognized '" + backend + "' backend");
}

}  // namespace sphinx::reactor
