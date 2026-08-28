/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sphinx::reactor {

class Message
{
public:
  virtual ~Message() = default;
};

// Messages are reference-counted so a failed non-deferred send leaves the
// caller's object untouched.  The callback receives the same owning handle,
// and no raw pointer crosses a reactor boundary.
using MessagePtr = std::shared_ptr<Message>;
using OnMessageFn = std::function<void(MessagePtr)>;

using TcpAcceptFn = std::function<void(int sockfd)>;

class EpollReactor;

struct Pollable
{
  virtual ~Pollable()
  {
  }
  virtual int fd() const = 0;
  virtual void on_pollin() = 0;
  virtual bool on_pollout() = 0;
};

class Socket : public Pollable
{
protected:
  int _sockfd;

public:
  explicit Socket(int sockfd);
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&&) = delete;
  Socket& operator=(Socket&&) = delete;
  virtual ~Socket();

  int fd() const;
  virtual bool send(const char* msg, size_t len) = 0;
};

class TcpListener : public Pollable
{
  int _sockfd;
  TcpAcceptFn _accept_fn;

public:
  explicit TcpListener(int sockfd, TcpAcceptFn&& accept_fn);
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  ~TcpListener();

  int fd() const override;

  void on_pollin() override;
  bool on_pollout() override;

private:
  void accept();
};

std::shared_ptr<TcpListener>
make_tcp_listener(const std::string& iface, int port, int backlog, TcpAcceptFn&& recv_fn);

class TcpSocket;

using TcpRecvFn = std::function<void(std::shared_ptr<TcpSocket>, std::string_view)>;

class TcpSocket
  : public Socket
  , public std::enable_shared_from_this<TcpSocket>
{
  TcpRecvFn _recv_fn;
  std::vector<char> _tx_buf;

public:
  explicit TcpSocket(int sockfd, TcpRecvFn&& recv_fn);
  ~TcpSocket();
  void set_tcp_nodelay(bool nodelay);
  bool send(const char* msg, size_t len) override;
  bool closed() const;
  void on_pollin() override;
  bool on_pollout() override;

private:
  bool _closed = false;
};

constexpr int max_nr_threads = 64;
constexpr size_t reactor_message_queue_size = 10000;

// Owns all state shared by a set of reactors.  A group is deliberately
// explicit: eventfds, bounded channels and overflow mailboxes live exactly as
// long as the group, rather than in process-global Reactor statics.
class ReactorGroup
{
  struct Channel;

  size_t _nr_threads;
  std::vector<int> _eventfds;
  std::unique_ptr<std::atomic<bool>[]> _thread_is_sleeping;
  std::vector<std::unique_ptr<Channel>> _channels;
  std::mutex _channels_mutex;

  Channel& channel(size_t destination, size_t source);
  void initialize_thread(size_t thread_id);
  int eventfd(size_t thread_id) const;
  bool is_thread_sleeping(size_t thread_id) const;
  void set_thread_sleeping(size_t thread_id, bool sleeping);

  friend class Reactor;
  friend class EpollReactor;

public:
  explicit ReactorGroup(size_t nr_threads);
  ~ReactorGroup();

  ReactorGroup(const ReactorGroup&) = delete;
  ReactorGroup& operator=(const ReactorGroup&) = delete;

  size_t nr_threads() const noexcept;
};

class Reactor
{
protected:
  std::shared_ptr<ReactorGroup> _group;
  int _efd = -1;
  size_t _thread_id;
  size_t _nr_threads;
  std::bitset<max_nr_threads> _pending_wakeups;
  OnMessageFn _on_message_fn;

public:
  static std::string default_backend();

  Reactor(size_t thread_id, std::shared_ptr<ReactorGroup> group, OnMessageFn&& on_message_fn);
  virtual ~Reactor() = default;
  size_t thread_id() const;
  size_t nr_threads() const;
  // A false return means that the message was not accepted and remains owned
  // by the caller.  send_msg reports bounded-queue backpressure; the deferred
  // variant additionally reports overflow-mailbox allocation failure.  The
  // caller must complete or retry a request when the deferred send fails.
  bool send_msg(size_t thread, const MessagePtr& message);
  bool send_msg_deferred(size_t thread, const MessagePtr& message);
  virtual void accept(std::shared_ptr<TcpListener>&& listener) = 0;
  virtual void recv(std::shared_ptr<Socket>&& socket) = 0;
  virtual void send(std::shared_ptr<Socket> socket) = 0;
  virtual void close(std::shared_ptr<Socket> socket) = 0;
  virtual void run() = 0;

protected:
  void wake_up_pending();
  void wake_up(size_t thread_id);
  bool send_msg_impl(size_t thread, const MessagePtr& message, bool defer_if_full);
  bool has_messages();
  bool poll_messages();
};

std::unique_ptr<Reactor>
make_reactor(const std::string& backend,
             size_t thread_id,
             std::shared_ptr<ReactorGroup> group,
             OnMessageFn&& on_message_fn);
} // namespace sphinx::reactor
