// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/reactor.h>

#include <set>
#include <string>
namespace sphinx {

// 默认网络与系统运行参数
inline constexpr int default_tcp_port = 11211;
inline constexpr const char* default_listen_addr = "0.0.0.0";
inline constexpr int default_memory_limit = 64;  // 默认内存配额（MB）
inline constexpr int default_segment_size = 2;   // 默认分段大小（MB）
inline constexpr int default_listen_backlog = 1024;
inline constexpr int default_nr_threads = 4;

// 服务端全局运行配置结构体（守护进程入口解析后传递给各工作线程使用）
struct Config {
  // 网络监听配置
  std::string listen_addr = default_listen_addr;
  int tcp_port = default_tcp_port;
  int listen_backlog = default_listen_backlog;

  // 内存分配与存储配置
  int memory_limit = default_memory_limit;  // 内存上限（兆字节）
  int segment_size = default_segment_size;  // 日志段大小（兆字节）

  // 并发与底层调度配置
  int nr_threads = default_nr_threads;                                // 工作线程数
  std::string backend = Reactor::default_backend();  // I/O 多路复用后端
  std::set<int> isolate_cpus;  // 需避开绑定的被隔离 CPU 核心 ID 集合
  bool sched_fifo = false;     // 是否启用 SCHED_FIFO 实时调度策略
};

// 解析命令行入参并填充配置结构体
Config parse_options(int argc, char* argv[], const std::string& program);

}  // namespace sphinx
