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

void run_server_thread(size_t thread_id, std::optional<int> cpu_id,
                       const sphinx::server::Config& config,
                       const std::shared_ptr<sphinx::stats::ServerStats>& stats,
                       const std::shared_ptr<sphinx::reactor::ReactorGroup>& reactor_group,
                       const std::shared_ptr<std::atomic_bool>& mget_queue_failure_used) {
  try {
    if (cpu_id) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(*cpu_id, &cpuset);
      auto error = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
      if (error != 0) {
        throw std::system_error(error, std::system_category(), "pthread_setaffinity_np");
      }
    }
    if (config.sched_fifo) {
      ::sched_param param = {};
      param.sched_priority = 1;
      auto error = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
      if (error != 0) {
        throw std::system_error(errno, std::system_category(), "pthread_setschedparam");
      }
    }

    auto memory_size = size_t(config.memory_limit) * 1024 * 1024;
    auto memory =
        sphinx::memory::Memory::mmap(memory_size / static_cast<size_t>(config.nr_threads));
    sphinx::logmem::LogConfig log_config;
    log_config.segment_size = size_t(config.segment_size) * 1024 * 1024;
    log_config.memory_ptr = reinterpret_cast<char*>(memory.addr());
    log_config.memory_size = memory.size();

    sphinx::server::Server server{log_config,    config.backend, thread_id,
                                  reactor_group, stats,          mget_queue_failure_used};
    server.serve(config);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n' << std::flush;
    std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
}

class CpuAffinity final {
  std::set<int> _isolated;
  std::optional<int> _next_id;

 public:
  explicit CpuAffinity(std::set<int> isolated) : _isolated{std::move(isolated)} {
  }

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

int main(int argc, char* argv[]) {
  try {
    std::string program = ::basename(argv[0]);
    auto config = sphinx::server::parse_options(argc, argv, program);
    auto stats = std::make_shared<sphinx::stats::ServerStats>(
        std::string{SPHINX_VERSION}, static_cast<uint64_t>(config.nr_threads),
        static_cast<uint64_t>(config.memory_limit) * 1024 * 1024);
    CpuAffinity cpu_affinity{config.isolate_cpus};
    auto reactor_group = std::make_shared<sphinx::reactor::ReactorGroup>(config.nr_threads);
    auto mget_queue_failure_used = std::make_shared<std::atomic_bool>(false);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(config.nr_threads));
    for (int thread_id = 0; thread_id < config.nr_threads; ++thread_id) {
      workers.emplace_back(run_server_thread, static_cast<size_t>(thread_id),
                           cpu_affinity.next_cpu_id(), std::cref(config), stats, reactor_group,
                           mget_queue_failure_used);
    }
    for (auto& worker : workers) {
      worker.join();
    }
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n' << std::flush;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
