// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/reactor.h>

#include <unordered_map>

namespace sphinx::reactor {

class EpollReactor : public Reactor
{
  std::unordered_map<int, std::shared_ptr<Pollable>> _pollables;
  std::unordered_map<int, uint32_t> _epoll_events;
  int _epollfd;

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
} // namespace sphinx::reactor
