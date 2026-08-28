// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

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

class Message {
 public:
  virtual ~Message() = default;
};

// 消息使用引用计数管理，因此非延迟发送失败时调用方对象保持不变。
// 回调接收同一个所有权句柄，反应堆边界之间不会传递裸指针。
using MessagePtr = std::shared_ptr<Message>;
using OnMessageFn = std::function<void(MessagePtr)>;

using TcpAcceptFn = std::function<void(int sockfd)>;

class EpollReactor;

struct Pollable {
  virtual ~Pollable() = default;
  virtual int fd() const = 0;
  virtual void on_pollin() = 0;
  virtual bool on_pollout() = 0;
};

class Socket : public Pollable {
 protected:
  int _sockfd;

 public:
  explicit Socket(int sockfd);
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&&) = delete;
  Socket& operator=(Socket&&) = delete;
  ~Socket() override;

  int fd() const override;
  virtual bool send(const char* msg, size_t len) = 0;
};

class TcpListener : public Pollable {
  int _sockfd;
  TcpAcceptFn _accept_fn;

 public:
  explicit TcpListener(int sockfd, TcpAcceptFn&& accept_fn);
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  ~TcpListener() override;

  int fd() const override;

  void on_pollin() override;
  bool on_pollout() override;

 private:
  void accept();
};

std::shared_ptr<TcpListener> make_tcp_listener(const std::string& iface, int port, int backlog,
                                               TcpAcceptFn&& accept_fn);

class TcpSocket;

using TcpRecvFn = std::function<void(std::shared_ptr<TcpSocket>, std::string_view)>;

class TcpSocket : public Socket, public std::enable_shared_from_this<TcpSocket> {
  TcpRecvFn _recv_fn;
  std::vector<char> _tx_buf;

 public:
  explicit TcpSocket(int sockfd, TcpRecvFn&& recv_fn);
  ~TcpSocket() override;
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

// 反应堆组拥有一组反应堆共享的全部状态。组边界是显式的：eventfd、有界通道
// 和溢出邮箱与组保持相同生命周期，而不是存放在进程级 Reactor 静态变量中。
class ReactorGroup {
  struct Channel;

  size_t _nr_threads;
  std::vector<int> _eventfds;
  std::vector<std::atomic<bool>> _thread_is_sleeping;
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

class Reactor {
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
  // 返回 false 表示消息未被接受，所有权仍归调用方。send_msg 报告有界队列回压；
  // 延迟版本还会报告溢出邮箱分配失败。延迟发送失败时调用方必须完成或重试请求。
  bool send_msg(size_t remote_id, const MessagePtr& message);
  bool send_msg_deferred(size_t remote_id, const MessagePtr& message);
  virtual void accept(std::shared_ptr<TcpListener>&& listener) = 0;
  virtual void recv(std::shared_ptr<Socket>&& socket) = 0;
  virtual void send(std::shared_ptr<Socket> socket) = 0;
  virtual void close(std::shared_ptr<Socket> socket) = 0;
  virtual void run() = 0;

 protected:
  void wake_up_pending();
  void wake_up(size_t thread_id);
  bool send_msg_impl(size_t remote_id, const MessagePtr& message, bool defer_if_full);
  bool has_messages();
  bool poll_messages();
};

std::unique_ptr<Reactor> make_reactor(const std::string& backend, size_t thread_id,
                                      std::shared_ptr<ReactorGroup> group,
                                      OnMessageFn&& on_message_fn);
}  // namespace sphinx::reactor
