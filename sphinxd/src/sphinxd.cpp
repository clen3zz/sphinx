// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <libgen.h>
#include <sched.h>
#include <sphinx/memory.h>
#include <sphinx/stats.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "server/config.h"
#include "server/server.h"

namespace {

// 工作线程主函数：负责各线程的 CPU 绑定、实时调度、内存分配、Server 实例化并启动事件循环
void run_server_thread(size_t thread_id, std::optional<int> cpu_id, const sphinx::Config& config,
                       const std::shared_ptr<sphinx::ServerStats>& stats,
                       const std::shared_ptr<sphinx::ReactorGroup>& reactor_group,
                       const std::shared_ptr<std::atomic_bool>& mget_queue_failure_used) {
  try {
    // 1. CPU 核心亲和性绑定（减少跨核调度与缓存失效）
    if (cpu_id) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(*cpu_id, &cpuset);
      auto error = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
      if (error != 0) {
        throw std::system_error(error, std::system_category(), "pthread_setaffinity_np");
      }
    }

    // 2. 实时调度策略配置（启用 SCHED_FIFO 以降低长尾延迟）
    if (config.sched_fifo) {
      ::sched_param param = {};
      param.sched_priority = 1;
      auto error = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
      if (error != 0) {
        throw std::system_error(errno, std::system_category(), "pthread_setschedparam");
      }
    }

    // 3. 线程私有内存分配（将总内存限额均分给各工作线程，通过 mmap 匿名映射）
    auto memory_size = static_cast<size_t>(config.memory_limit) * 1024 * 1024;
    auto memory = sphinx::Memory::mmap(memory_size / static_cast<size_t>(config.nr_threads));

    // 4. 日志型内存存储（Log-structured Memory）配置
    sphinx::LogConfig log_config;
    log_config.segment_size = static_cast<size_t>(config.segment_size) * 1024 * 1024;
    log_config.memory_ptr = static_cast<char*>(memory.addr());
    log_config.memory_size = memory.size();

    // 5. 初始化 Server 实例并启动事件循环（监听端口并处理请求）
    sphinx::Server server{log_config,    config.backend, thread_id,
                          reactor_group, stats,          mget_queue_failure_used};
    server.serve(config);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n' << std::flush;
    std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
}

// CPU 亲和性分配器：按顺序为工作线程分配 CPU 核心编号，自动跳过被隔离的核心
class CpuAffinity final {
  std::set<int> _isolated;
  std::optional<int> _next_id;

 public:
  explicit CpuAffinity(std::set<int> isolated) : _isolated{std::move(isolated)} {
  }

  // 获取下一个可用的 CPU 核心编号
  int next_cpu_id() {
    int id = _next_id.value_or(0);
    while (_isolated.count(id) != 0) {
      ++id;
    }
    _next_id = id + 1;
    return id;
  }
};

}  // namespace

// 守护进程入口
int main(int argc, char* argv[]) {
  try {
    // 1. 命令行参数解析
    std::string program = ::basename(argv[0]);
    auto config = sphinx::parse_options(argc, argv, program);

    // 2. 全局统计指标初始化
    auto stats = std::make_shared<sphinx::ServerStats>(
        std::string{SPHINX_VERSION}, static_cast<uint64_t>(config.nr_threads),
        static_cast<uint64_t>(config.memory_limit) * 1024 * 1024);

    // 3. CPU 亲和性分配器初始化（传入用户指定的隔离核心列表）
    CpuAffinity cpu_affinity{config.isolate_cpus};

    // 4. 线程间通信与协同组件初始化（Reactor 分组与全局多键获取失败标记）
    auto reactor_group = std::make_shared<sphinx::ReactorGroup>(config.nr_threads);
    auto mget_queue_failure_used = std::make_shared<std::atomic_bool>(false);

    // 5. 创建并启动工作线程池
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(config.nr_threads));
    for (int thread_id = 0; thread_id < config.nr_threads; ++thread_id) {
      workers.emplace_back(run_server_thread, static_cast<size_t>(thread_id),
                           cpu_affinity.next_cpu_id(), std::cref(config), stats, reactor_group,
                           mget_queue_failure_used);
    }

    // 6. 等待所有工作线程执行结束
    for (auto& worker : workers) {
      worker.join();
    }
  } catch (const std::exception& error) {
    // 7. 顶层异常捕获与错误退出
    std::cerr << "error: " << error.what() << '\n' << std::flush;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
