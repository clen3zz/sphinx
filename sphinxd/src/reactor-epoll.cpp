// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/reactor-epoll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <stdexcept>
#include <system_error>

namespace sphinx {
namespace {

// Eventfd 可轮询对象包装：用于跨线程唤醒 Reactor 事件循环
class Eventfd : public Pollable {
  int _efd;

 public:
  explicit Eventfd(int efd) : _efd{efd} {
  }

  int fd() const override {
    return _efd;
  }

  // 读就绪时清空 eventfd 计数器
  void on_pollin() override {
    eventfd_t unused;
    if (::eventfd_read(_efd, &unused) < 0 && errno != EAGAIN && errno != EINTR) {
      throw std::system_error(errno, std::system_category(), "eventfd_read");
    }
  }

  bool on_pollout() override {
    return false;
  }
};

}  // namespace

// 基于 epoll 的 Reactor 实现构造函数
EpollReactor::EpollReactor(size_t thread_id, std::shared_ptr<ReactorGroup> group,
                           OnMessageFn&& on_message_fn)
    : Reactor{thread_id, std::move(group), std::move(on_message_fn)}, _epollfd{::epoll_create1(0)} {
  // 1. 创建 epoll 句柄
  if (_epollfd < 0) {
    throw std::system_error(errno, std::system_category(), "epoll_create1");
  }

  // 2. 将跨线程唤醒用的 eventfd 注册进 epoll 监听
  try {
    auto eventfd = std::make_shared<Eventfd>(_efd);
    update_epoll(eventfd.get(), EPOLLIN);
    _pollables.emplace(eventfd->fd(), eventfd);
  } catch (...) {
    ::close(_epollfd);
    _epollfd = -1;
    throw;
  }
}

// 快速构造函数：自动创建 ReactorGroup
EpollReactor::EpollReactor(size_t thread_id, size_t nr_threads, OnMessageFn&& on_message_fn)
    : EpollReactor{thread_id, std::make_shared<ReactorGroup>(nr_threads),
                   std::move(on_message_fn)} {
}

// 析构函数：释放 epoll 句柄
EpollReactor::~EpollReactor() {
  if (_epollfd >= 0) {
    ::close(_epollfd);
  }
}

// 注册 TCP 监听器（关注读事件）
void EpollReactor::accept(std::shared_ptr<TcpListener>&& listener) {
  if (!listener) {
    throw std::invalid_argument("cannot register a null listener");
  }

  update_epoll(listener.get(), EPOLLIN);
  _pollables.emplace(listener->fd(), std::move(listener));
}

// 注册套接字读事件（数据接收）
void EpollReactor::recv(std::shared_ptr<Socket>&& socket) {
  if (!socket) {
    throw std::invalid_argument("cannot register a null socket");
  }

  update_epoll(socket.get(), EPOLLIN);
  _pollables.emplace(socket->fd(), std::move(socket));
}

// 注册套接字写事件（异步排队写出）
void EpollReactor::send(std::shared_ptr<Socket> socket) {
  if (!socket) {
    throw std::invalid_argument("cannot register a null socket");
  }

  update_epoll(socket.get(), EPOLLIN | EPOLLOUT);
  _pollables.emplace(socket->fd(), socket);
}

// 注销套接字并执行安全关闭
void EpollReactor::close(std::shared_ptr<Socket> socket) {
  if (!socket) {
    return;
  }

  // 1. 从 epoll 监听树中移除
  _epoll_events.erase(socket->fd());
  if (::epoll_ctl(_epollfd, EPOLL_CTL_DEL, socket->fd(), nullptr) < 0 && errno != ENOENT &&
      errno != EBADF) {
    throw std::system_error(errno, std::system_category(), "epoll_ctl");
  }

  // 2. 双向关闭套接字传输
  if (::shutdown(socket->fd(), SHUT_RDWR) < 0) {
    if (errno != ENOTCONN && errno != EINVAL && errno != EBADF) {
      throw std::system_error(errno, std::system_category(), "close");
    }
  }

  // 3. 从内部可轮询集合中清除
  _pollables.erase(socket->fd());
}

// Reactor 事件驱动核心主循环
void EpollReactor::run() {
  std::array<epoll_event, 128> events;

  for (;;) {
    // 1. 唤醒其他处于睡眠状态的 Reactor 线程
    wake_up_pending();

    int nr_events = 0;

    // 2. 消费线程邮箱消息；若有未处理消息则非阻塞轮询，否则准备挂起休眠
    if (poll_messages()) {
      // 存在未处理消息：推测后续可能还有新请求，采用 0 超时非阻塞探测，避免睡眠开销
      nr_events = ::epoll_wait(_epollfd, events.data(), events.size(), 0);
    } else {
      // 无消息：标记当前线程即将休眠
      _group->set_thread_sleeping(_thread_id, true);

      // 双重检查防竞争：若在置睡眠标志时收到新消息，则立即重试不进入睡眠
      if (has_messages()) {
        _group->set_thread_sleeping(_thread_id, false);
        continue;
      }

      // 阻塞等待 I/O 事件或被 eventfd 唤醒
      nr_events = ::epoll_wait(_epollfd, events.data(), events.size(), -1);
      _group->set_thread_sleeping(_thread_id, false);
    }

    // 3. 信号中断忽略
    if (nr_events == -1 && errno == EINTR) {
      continue;
    }

    if (nr_events < 0) {
      throw std::system_error(errno, std::system_category(), "epoll_wait");
    }

    // 4. 遍历并分发触发的 I/O 就绪事件
    for (int i = 0; i < nr_events; i++) {
      epoll_event* event = &events[static_cast<size_t>(i)];
      auto fd = event->data.fd;
      auto it = _pollables.find(fd);

      if (it == _pollables.end()) {
        ::epoll_ctl(_epollfd, EPOLL_CTL_DEL, fd, nullptr);
        continue;
      }

      auto pollable = it->second;

      // 读事件或异常断开事件处理
      if ((event->events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        pollable->on_pollin();
      }

      // 若在 on_pollin 处理过程中该 fd 已被注销，跳过后续写事件
      if (_pollables.find(fd) == _pollables.end()) {
        continue;
      }

      // 写就绪事件处理
      if ((event->events & EPOLLOUT) != 0) {
        if (pollable->on_pollout()) {
          // 发送缓冲区已全部清空，退回仅关注读事件
          update_epoll(pollable.get(), EPOLLIN);
        }
      }
    }
  }
}

// 增量添加或修改 epoll 监听的事件类型
void EpollReactor::update_epoll(Pollable* pollable, uint32_t events) {
  int op = EPOLL_CTL_ADD;
  auto it = _epoll_events.find(pollable->fd());

  if (it != _epoll_events.end()) {
    if (events == it->second) {
      return;
    }
    op = EPOLL_CTL_MOD;
  }

  ::epoll_event ev = {};
  ev.data.fd = pollable->fd();
  ev.events = events | EPOLLRDHUP;

  if (::epoll_ctl(_epollfd, op, pollable->fd(), &ev) < 0) {
    throw std::system_error(errno, std::system_category(), "epoll_ctl");
  }

  _epoll_events.insert_or_assign(pollable->fd(), events);
}

}  // namespace sphinx
