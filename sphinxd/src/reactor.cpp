// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/reactor.h>

#include <sphinx/reactor-epoll.h>
#include <sphinx/spsc_queue.h>

#include <netdb.h>
#include <netinet/tcp.h>
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

Socket::Socket(int sockfd)
  : _sockfd{sockfd}
{
}

Socket::~Socket()
{
  ::close(_sockfd);
}

int
Socket::fd() const
{
  return _sockfd;
}

TcpListener::TcpListener(int sockfd, TcpAcceptFn&& accept_fn)
  : _sockfd{sockfd}
  , _accept_fn{accept_fn}
{
}

TcpListener::~TcpListener()
{
  ::close(_sockfd);
}

void
TcpListener::on_pollin()
{
  accept();
}

bool
TcpListener::on_pollout()
{
  return true;
}

void
TcpListener::accept()
{
  for (;;) {
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
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    throw std::system_error(errno, std::system_category(), "accept4");
  }
}

int
TcpListener::fd() const
{
  return _sockfd;
}

static addrinfo*
lookup_addresses(const std::string& iface, int port, int sock_type)
{
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
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

std::shared_ptr<TcpListener>
make_tcp_listener(const std::string& iface, int port, int backlog, TcpAcceptFn&& accept_fn)
{
  auto* addresses = lookup_addresses(iface, port, SOCK_STREAM);
  for (addrinfo* rp = addresses; rp != nullptr; rp = rp->ai_next) {
    int sockfd =
      ::socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, rp->ai_protocol);
    if (sockfd < 0) {
      continue;
    }
    int one = 1;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    if (::bind(sockfd, rp->ai_addr, rp->ai_addrlen) < 0) {
      ::close(sockfd);
      continue;
    }
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

TcpSocket::TcpSocket(int sockfd, TcpRecvFn&& recv_fn)
  : Socket{sockfd}
  , _recv_fn{recv_fn}
{
}

TcpSocket::~TcpSocket() = default;

void
TcpSocket::set_tcp_nodelay(bool nodelay)
{
  int value = nodelay ? 1 : 0;
  if (setsockopt(_sockfd, SOL_TCP, TCP_NODELAY, &value, sizeof(value)) < 0) {
    throw std::system_error(errno, std::system_category(), "setsockopt");
  }
}

bool
TcpSocket::closed() const
{
  return _closed;
}

bool
TcpSocket::send(const char* msg, size_t len)
{
  if (_closed) {
    return true;
  }
  if (len == 0) {
    return true;
  }
  if (!_tx_buf.empty()) {
    _tx_buf.insert(_tx_buf.end(), msg, msg + len);
    return false;
  }
  ssize_t nr;
  do {
    nr = ::send(_sockfd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (nr < 0 && errno == EINTR);
  if ((nr < 0) && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN || errno == EBADF)) {
    _closed = true;
    return true;
  }
  if ((nr < 0) && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    _tx_buf.insert(_tx_buf.end(), msg, msg + len);
    return false;
  }
  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "send");
  }
  if (nr == 0 || static_cast<size_t>(nr) < len) {
    _tx_buf.insert(_tx_buf.end(), msg + nr, msg + len);
    return false;
  }
  return true;
}

void
TcpSocket::on_pollin()
{
  constexpr size_t rx_buf_size = size_t{256} * 1024;
  std::array<char, rx_buf_size> rx_buf;
  for (;;) {
    ssize_t nr = ::recv(_sockfd, rx_buf.data(), rx_buf.size(), MSG_DONTWAIT);
    if (nr > 0) {
      _recv_fn(this->shared_from_this(),
               std::string_view{rx_buf.data(), static_cast<std::string_view::size_type>(nr)});
      return;
    }
    if (nr == 0) {
      _closed = true;
      _recv_fn(this->shared_from_this(), std::string_view{});
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    if (errno == ECONNRESET || errno == ENOTCONN || errno == EBADF) {
      _closed = true;
      _recv_fn(this->shared_from_this(), std::string_view{});
      return;
    }
    throw std::system_error(errno, std::system_category(), "recv");
  }
}

bool
TcpSocket::on_pollout()
{
  if (_closed || _tx_buf.empty()) {
    return true;
  }
  ssize_t nr;
  do {
    nr = ::send(_sockfd, _tx_buf.data(), _tx_buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (nr < 0 && errno == EINTR);
  if ((nr < 0) && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN || errno == EBADF)) {
    _closed = true;
    return true;
  }
  if ((nr < 0) && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return false;
  }
  if (nr < 0) {
    throw std::system_error(errno, std::system_category(), "send");
  }
  if (nr == 0) {
    return false;
  }
  _tx_buf.erase(_tx_buf.begin(), _tx_buf.begin() + nr);
  return _tx_buf.empty();
}

struct ReactorGroup::Channel
{
  sphinx::spsc::Queue<MessagePtr, reactor_message_queue_size> queue;
  std::mutex overflow_mutex;
  std::deque<MessagePtr> overflow;
};

static size_t
checked_thread_count(size_t nr_threads)
{
  if (nr_threads == 0 || nr_threads > max_nr_threads) {
    throw std::invalid_argument("invalid reactor thread count");
  }
  return nr_threads;
}

ReactorGroup::ReactorGroup(size_t nr_threads)
  : _nr_threads{checked_thread_count(nr_threads)}
  , _eventfds(_nr_threads, -1)
  , _thread_is_sleeping(_nr_threads)
  , _channels(_nr_threads * _nr_threads)
{
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

ReactorGroup::~ReactorGroup()
{
  for (auto& efd : _eventfds) {
    if (efd >= 0) {
      ::close(efd);
      efd = -1;
    }
  }
}

size_t
ReactorGroup::nr_threads() const noexcept
{
  return _nr_threads;
}

ReactorGroup::Channel&
ReactorGroup::channel(size_t destination, size_t source)
{
  if (destination >= _nr_threads || source >= _nr_threads) {
    throw std::invalid_argument("invalid reactor message target");
  }
  auto& slot = _channels[(destination * _nr_threads) + source];
  if (!slot) {
    throw std::logic_error("reactor channel is not initialized");
  }
  return *slot;
}

void
ReactorGroup::initialize_thread(size_t thread_id)
{
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor thread id");
  }
  std::scoped_lock lock{_channels_mutex};
  for (size_t peer = 0; peer < _nr_threads; peer++) {
    if (peer == thread_id) {
      continue;
    }
    auto& outgoing = _channels[(peer * _nr_threads) + thread_id];
    if (!outgoing) {
      outgoing = std::make_unique<Channel>();
    }
    auto& incoming = _channels[(thread_id * _nr_threads) + peer];
    if (!incoming) {
      incoming = std::make_unique<Channel>();
    }
  }
}

int
ReactorGroup::eventfd(size_t thread_id) const
{
  if (thread_id >= _nr_threads || _eventfds[thread_id] < 0) {
    throw std::invalid_argument("invalid reactor wakeup target");
  }
  return _eventfds[thread_id];
}

bool
ReactorGroup::is_thread_sleeping(size_t thread_id) const
{
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor wakeup target");
  }
  return _thread_is_sleeping[thread_id].load(std::memory_order_seq_cst);
}

void
ReactorGroup::set_thread_sleeping(size_t thread_id, bool sleeping)
{
  if (thread_id >= _nr_threads) {
    throw std::invalid_argument("invalid reactor wakeup target");
  }
  _thread_is_sleeping[thread_id].store(sleeping, std::memory_order_seq_cst);
}

std::string
Reactor::default_backend()
{
  return "epoll";
}

Reactor::Reactor(size_t thread_id, std::shared_ptr<ReactorGroup> group, OnMessageFn&& on_message_fn)
  : _group{std::move(group)}
  , _thread_id{thread_id}
  , _nr_threads{0}
  , _on_message_fn{std::move(on_message_fn)}
{
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

size_t
Reactor::thread_id() const
{
  return _thread_id;
}

size_t
Reactor::nr_threads() const
{
  return _nr_threads;
}

bool
Reactor::send_msg(size_t remote_id, const MessagePtr& message)
{
  return send_msg_impl(remote_id, message, false);
}

bool
Reactor::send_msg_deferred(size_t remote_id, const MessagePtr& message)
{
  return send_msg_impl(remote_id, message, true);
}

bool
Reactor::send_msg_impl(size_t remote_id, const MessagePtr& message, bool defer_if_full)
{
  if (remote_id == _thread_id) {
    throw std::invalid_argument("Attempting to send message to self");
  }
  if (remote_id >= _nr_threads || !message) {
    throw std::invalid_argument("invalid reactor message target");
  }
  auto& channel = _group->channel(remote_id, _thread_id);
  {
    std::scoped_lock lock{channel.overflow_mutex};
    // 一旦出现溢出消息，后续消息也放入溢出邮箱，确保目的端观察到的顺序
    // 与有界环形队列保持一致。
    if (channel.overflow.empty() && channel.queue.try_to_emplace(message)) {
      // 有界队列已接受该消息。
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
  _pending_wakeups.set(remote_id);
  return true;
}

void
Reactor::wake_up_pending()
{
  for (size_t id = 0; id < _pending_wakeups.size(); id++) {
    if (_pending_wakeups.test(id) && _group->is_thread_sleeping(id)) {
      _group->set_thread_sleeping(id, false);
      wake_up(id);
    }
  }
  _pending_wakeups.reset();
}

void
Reactor::wake_up(size_t thread_id)
{
  auto efd = _group->eventfd(thread_id);
  if (::eventfd_write(efd, 1) < 0) {
    if (errno == EAGAIN) {
      return;
    }
    throw std::system_error(errno, std::system_category(), "eventfd_write");
  }
}

bool
Reactor::has_messages()
{
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

bool
Reactor::poll_messages()
{
  bool received = false;
  for (size_t other = 0; other < _nr_threads; other++) {
    if (other == _thread_id) {
      continue;
    }
    auto& channel = _group->channel(_thread_id, other);
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

std::unique_ptr<Reactor>
make_reactor(const std::string& backend,
             size_t thread_id,
             std::shared_ptr<ReactorGroup> group,
             OnMessageFn&& on_message_fn)
{
  if (backend == "epoll") {
    return std::make_unique<EpollReactor>(thread_id, std::move(group), std::move(on_message_fn));
  }
  throw std::invalid_argument("unrecognized '" + backend + "' backend");
}

} // namespace sphinx::reactor
