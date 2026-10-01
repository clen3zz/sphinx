// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <libgen.h>
#include <sphinx/memory.h>
#include <sphinx/stats.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "server/config.h"
#include "server/server.h"

namespace {

// 工作线程主函数：分配本线程的存储内存，创建 Server 并启动事件循环。
void run_server_thread(size_t thread_id, const sphinx::Config& config,
                       const std::shared_ptr<sphinx::ServerStats>& stats,
                       const std::shared_ptr<sphinx::ReactorGroup>& reactor_group,
                       const std::shared_ptr<std::atomic_bool>& mget_queue_failure_used) {
  try {
    // 线程私有内存分配（将总内存限额均分给各工作线程，通过 mmap 匿名映射）
    auto memory_size = static_cast<size_t>(config.memory_limit) * 1024 * 1024;
    auto memory = sphinx::Memory::mmap(memory_size / static_cast<size_t>(config.nr_threads));

    // 日志型内存存储（Log-structured Memory）配置
    sphinx::LogConfig log_config;
    log_config.segment_size = static_cast<size_t>(config.segment_size) * 1024 * 1024;
    log_config.memory_ptr = static_cast<char*>(memory.addr());
    log_config.memory_size = memory.size();

    // 初始化 Server 实例并启动事件循环（监听端口并处理请求）
    sphinx::Server server{log_config, thread_id, reactor_group, stats, mget_queue_failure_used};
    server.serve(config);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n' << std::flush;
    std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
}

}  // namespace

// 守护进程入口
int main(int argc, char* argv[]) {
  try {
    // 1. 命令行参数解析
    std::string const program = basename(argv[0]);
    auto config = sphinx::parse_options(argc, argv, program);

    // 2. 全局统计指标初始化
    auto stats = std::make_shared<sphinx::ServerStats>(
        std::string{SPHINX_VERSION}, static_cast<uint64_t>(config.nr_threads),
        static_cast<uint64_t>(config.memory_limit) * 1024 * 1024);

    // 线程间通信与协同组件初始化（Reactor 分组与全局多键获取失败标记）
    auto reactor_group = std::make_shared<sphinx::ReactorGroup>(config.nr_threads);
    auto mget_queue_failure_used = std::make_shared<std::atomic_bool>(false);

    // 创建并启动工作线程池
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(config.nr_threads));
    for (int thread_id = 0; thread_id < config.nr_threads; ++thread_id) {
      workers.emplace_back(run_server_thread, static_cast<size_t>(thread_id), std::cref(config),
                           stats, reactor_group, mget_queue_failure_used);
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
