// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/bootstrap/product_config.h>
#include <sphinx/product/http/product_http.h>

#include <cassert>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "product_http_routes.h"

namespace sphinx {
namespace {

// 资源归工作线程栈上的 WorkerContext 所有，此指针只用于路由访问。
thread_local ProductService* worker_service = nullptr;

class ProductWorkerQueue final : public httplib::TaskQueue {
 public:
  ProductWorkerQueue(std::size_t count, const ProductWorkerRunner& run_worker,
                     httplib::Server& server, std::exception_ptr& failure)
      : _run_worker{run_worker}, _server{server}, _failure{failure} {
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
      shutdown();
      throw;
    }
  }
  ~ProductWorkerQueue() override { shutdown(); }

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

 private:
  void work() {
    try {
      _run_worker([this](ProductService& service) {
        worker_service = &service;
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
      });
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

  const ProductWorkerRunner& _run_worker;
  httplib::Server& _server;
  std::exception_ptr& _failure;
  std::vector<std::thread> _threads;
  std::deque<std::function<void()>> _jobs;
  std::mutex _mutex;
  std::condition_variable _ready;
  std::size_t _started = 0;
  bool _stopping = false;
};

void validate_http_config(const ProductHttpConfig& config) {
  if (config.bind_address.empty() || config.port == 0 || config.worker_count == 0 ||
      config.worker_count > 64) {
    throw std::invalid_argument{"invalid product HTTP configuration"};
  }
}

}  // namespace

ProductHttpServer::ProductHttpServer(const ProductRuntimeConfig& config,
                                     ProductWorkerRunner run_worker,
                                     const ProductSharedState& shared)
    : _config{config}, _run_worker{std::move(run_worker)}, _shared{shared} {
  validate_http_config(_config.http);
  if (!_run_worker) {
    throw std::invalid_argument{"product worker runner is empty"};
  }
  _server.set_payload_max_length(65536);
  _server.set_read_timeout(2, 0);
  _server.set_write_timeout(2, 0);
  _server.new_task_queue = [this] {
    return new ProductWorkerQueue{_config.http.worker_count, _run_worker, _server, _worker_failure};
  };
}

ProductHttpServer::~ProductHttpServer() = default;

ProductService& current_product_service() {
  assert(worker_service != nullptr);
  return *worker_service;
}

bool ProductHttpServer::serve() {
  {
    std::lock_guard lock{_state_mutex};
    if (_serve_called) {
      throw std::logic_error{"ProductHttpServer::serve may only be called once"};
    }
    _serve_called = true;
    if (_stopping) {
      return true;
    }
  }

  install_product_routes(_server, _shared, _config);

  {
    std::lock_guard lock{_state_mutex};
    if (_stopping) {
      return true;
    }
    if (!_server.bind_to_port(_config.http.bind_address, _config.http.port)) {
      return false;
    }
  }

  const bool listen_result = _server.listen_after_bind();
  // listen 返回前队列已 join 所有线程，故此时读取工作线程异常无需额外同步。
  if (_worker_failure) {
    std::rethrow_exception(_worker_failure);
  }
  std::lock_guard lock{_state_mutex};
  return _stopping || listen_result;
}

void ProductHttpServer::stop() noexcept {
  std::lock_guard lock{_state_mutex};
  _stopping = true;
  _server.stop();
}

}  // namespace sphinx
