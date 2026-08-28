// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/reactor.h>

#include <set>
#include <string>
namespace sphinx::server {

inline constexpr int default_tcp_port = 11211;
inline constexpr const char* default_listen_addr = "0.0.0.0";
inline constexpr int default_memory_limit = 64;
inline constexpr int default_segment_size = 2;
inline constexpr int default_listen_backlog = 1024;
inline constexpr int default_nr_threads = 4;

// Values parsed by the daemon entry point and consumed by every worker.
struct Config
{
  std::string listen_addr = default_listen_addr;
  int tcp_port = default_tcp_port;
  int memory_limit = default_memory_limit; // MB
  int segment_size = default_segment_size; // MB
  int listen_backlog = default_listen_backlog;
  int nr_threads = default_nr_threads;
  std::string backend = sphinx::reactor::Reactor::default_backend();
  std::set<int> isolate_cpus;
  bool sched_fifo = false;
};
Config
parse_options(int argc, char* argv[], const std::string& program);
} // namespace sphinx::server
