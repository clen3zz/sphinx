// SPDX-License-Identifier: Apache-2.0
#include <application/product_service.h>
#include <backends/mysql/mysql_product_store.h>
#include <backends/mysql/mysql_runtime.h>
#include <bootstrap/product_worker.h>

#include <cassert>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {

namespace {

struct WorkerContext final {
  WorkerContext(const ProductRuntimeConfig& config, ProductSharedState& shared)
      : store{config.mysql},
        cache{make_product_cache(config.cache)},
        service{store, *cache, shared, config.cache_policy} {}

  // guard 最先构造、最后析构，覆盖本线程所有 MySQL 对象的生命周期。
  [[maybe_unused]] MySqlThreadGuard thread_guard;
  MySqlProductStore store;
  std::unique_ptr<ProductCache> cache;
  ProductService service;
};

// 资源归工作线程栈上的 WorkerContext 所有，此指针只用于路由访问。
thread_local ProductService* worker_service = nullptr;

class ProductWorkerQueue final : public httplib::TaskQueue {
 public:
  ProductWorkerQueue(const ProductRuntimeConfig& config, ProductSharedState& shared,
                     httplib::Server& server)
      : _config{config}, _shared{shared}, _server{server} {
    const auto count = config.http.worker_count;
    try {
      _threads.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        _threads.emplace_back([this] { work(); });
      }
      std::unique_lock lock{_mutex};
      _ready.wait(lock, [&] { return _started == count || _failure; });
      if (_failure) {
        std::rethrow_exception(_failure);
      }
    } catch (...) {
      stop_and_join();
      throw;
    }
  }
  ~ProductWorkerQueue() override { stop_and_join(); }

  bool enqueue(std::function<void()> job) override {
    std::lock_guard lock{_mutex};
    if (_stopping || _jobs.size() >= 256) {
      return false;
    }
    _jobs.push_back(std::move(job));
    _ready.notify_one();
    return true;
  }
  void shutdown() override {
    stop_and_join();
    if (_failure) {
      std::rethrow_exception(_failure);
    }
  }

 private:
  void stop_and_join() {
    {
      std::lock_guard lock{_mutex};
      _stopping = true;
    }
    _ready.notify_all();
    for (auto& thread : _threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void work() {
    try {
      WorkerContext worker{_config, _shared};
      worker_service = &worker.service;
      {
        std::lock_guard lock{_mutex};
        ++_started;
      }
      _ready.notify_all();
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock{_mutex};
          _ready.wait(lock, [this] { return _stopping || !_jobs.empty(); });
          if (_stopping && _jobs.empty()) {
            break;
          }
          job = std::move(_jobs.front());
          _jobs.pop_front();
        }
        job();
      }
      worker_service = nullptr;
    } catch (...) {
      worker_service = nullptr;
      {
        std::lock_guard lock{_mutex};
        _failure = std::current_exception();
        _stopping = true;
      }
      _ready.notify_all();
      _server.stop();
    }
  }

  const ProductRuntimeConfig& _config;
  ProductSharedState& _shared;
  httplib::Server& _server;
  std::exception_ptr _failure;
  std::vector<std::thread> _threads;
  std::deque<std::function<void()>> _jobs;
  std::mutex _mutex;
  std::condition_variable _ready;
  std::size_t _started = 0;
  bool _stopping = false;
};

}  // namespace

httplib::TaskQueue* make_product_worker_queue(const ProductRuntimeConfig& config,
                                              ProductSharedState& shared, httplib::Server& server) {
  return new ProductWorkerQueue{config, shared, server};
}

ProductService& current_product_service() {
  assert(worker_service != nullptr);
  return *worker_service;
}

}  // namespace sphinx
