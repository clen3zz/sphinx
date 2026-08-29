// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/reactor.h>

#include <unordered_map>

namespace sphinx {

// 基于 Linux epoll 机制的高性能 Reactor 事件驱动实现
class EpollReactor : public Reactor {
  std::unordered_map<int, std::shared_ptr<Pollable>>
      _pollables;  // 被监听的文件描述符到对应 Pollable 可轮询对象的映射表
  std::unordered_map<int, uint32_t> _epoll_events;  // 各文件描述符当前注册在 epoll 监听中的事件掩码
  int _epollfd;                                     // Linux epoll 实例的文件描述符

 public:
  EpollReactor(size_t thread_id, std::shared_ptr<ReactorGroup> group, OnMessageFn&& on_message_fn);
  EpollReactor(size_t thread_id, size_t nr_threads, OnMessageFn&& on_message_fn);
  ~EpollReactor() override;
  void accept(std::shared_ptr<TcpListener>&& listener) override;
  void recv(std::shared_ptr<Socket>&& socket) override;
  void send(std::shared_ptr<Socket> socket) override;
  void close(std::shared_ptr<Socket> socket) override;
  void run() override;

 private:
  void update_epoll(Pollable* pollable, uint32_t events);
};
}  // namespace sphinx
