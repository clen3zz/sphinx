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

namespace sphinx::reactor {

class Eventfd : public Pollable
{
  int _efd;

public:
  explicit Eventfd(int efd)
    : _efd{efd}
  {
  }

  int fd() const override
  {
    return _efd;
  }

  void on_pollin() override
  {
    eventfd_t unused;
    if (::eventfd_read(_efd, &unused) < 0 && errno != EAGAIN && errno != EINTR) {
      throw std::system_error(errno, std::system_category(), "eventfd_read");
    }
  }
  bool on_pollout() override
  {
    return false;
  }
};

EpollReactor::EpollReactor(size_t thread_id,
                           std::shared_ptr<ReactorGroup> group,
                           OnMessageFn&& on_message_fn)
  : Reactor{thread_id, std::move(group), std::move(on_message_fn)}
  , _epollfd{::epoll_create1(0)}
{
  if (_epollfd < 0) {
    throw std::system_error(errno, std::system_category(), "epoll_create1");
  }
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

EpollReactor::EpollReactor(size_t thread_id, size_t nr_threads, OnMessageFn&& on_message_fn)
  : EpollReactor{thread_id, std::make_shared<ReactorGroup>(nr_threads), std::move(on_message_fn)}
{
}

EpollReactor::~EpollReactor()
{
  if (_epollfd >= 0) {
    ::close(_epollfd);
  }
}

void
EpollReactor::accept(std::shared_ptr<TcpListener>&& listener)
{
  if (!listener) {
    throw std::invalid_argument("cannot register a null listener");
  }
  update_epoll(listener.get(), EPOLLIN);
  _pollables.emplace(listener->fd(), std::move(listener));
}

void
EpollReactor::recv(std::shared_ptr<Socket>&& socket)
{
  if (!socket) {
    throw std::invalid_argument("cannot register a null socket");
  }
  update_epoll(socket.get(), EPOLLIN);
  _pollables.emplace(socket->fd(), std::move(socket));
}

void
EpollReactor::send(std::shared_ptr<Socket> socket)
{
  if (!socket) {
    throw std::invalid_argument("cannot register a null socket");
  }
  update_epoll(socket.get(), EPOLLIN | EPOLLOUT);
  _pollables.emplace(socket->fd(), socket);
}

void
EpollReactor::close(std::shared_ptr<Socket> socket)
{
  if (!socket) {
    return;
  }
  _epoll_events.erase(socket->fd());
  if (::epoll_ctl(_epollfd, EPOLL_CTL_DEL, socket->fd(), nullptr) < 0 && errno != ENOENT &&
      errno != EBADF) {
    throw std::system_error(errno, std::system_category(), "epoll_ctl");
  }
  if (::shutdown(socket->fd(), SHUT_RDWR) < 0) {
    if (errno != ENOTCONN && errno != EINVAL && errno != EBADF) {
      throw std::system_error(errno, std::system_category(), "close");
    }
  }
  _pollables.erase(socket->fd());
}

void
EpollReactor::run()
{
  std::array<epoll_event, 128> events;
  for (;;) {
    wake_up_pending();
    int nr_events = 0;
    if (poll_messages()) {
      // We had messages, speculate that there will be more message, and
      // therefore do not sleep:
      nr_events = ::epoll_wait(_epollfd, events.data(), events.size(), 0);
    } else {
      // No messages, attempt to sleep:
      _group->set_thread_sleeping(_thread_id, true);
      if (has_messages()) {
        // Raced with producers, restart:
        _group->set_thread_sleeping(_thread_id, false);
        continue;
      }
      nr_events = ::epoll_wait(_epollfd, events.data(), events.size(), -1);
      _group->set_thread_sleeping(_thread_id, false);
    }
    if (nr_events == -1 && errno == EINTR) {
      continue;
    }
    if (nr_events < 0) {
      throw std::system_error(errno, std::system_category(), "epoll_wait");
    }
    for (int i = 0; i < nr_events; i++) {
      epoll_event* event = &events[static_cast<size_t>(i)];
      auto fd = event->data.fd;
      auto it = _pollables.find(fd);
      if (it == _pollables.end()) {
        ::epoll_ctl(_epollfd, EPOLL_CTL_DEL, fd, nullptr);
        continue;
      }
      auto pollable = it->second;
      if ((event->events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        pollable->on_pollin();
      }
      if (_pollables.find(fd) == _pollables.end()) {
        continue;
      }
      if ((event->events & EPOLLOUT) != 0) {
        if (pollable->on_pollout()) {
          update_epoll(pollable.get(), EPOLLIN);
        }
      }
    }
  }
}

void
EpollReactor::update_epoll(Pollable* pollable, uint32_t events)
{
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
} // namespace sphinx::reactor
