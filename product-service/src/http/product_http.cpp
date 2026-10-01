// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/http/product_http.h>

#include <mutex>
#include <stdexcept>
#include <utility>

#include "product_http_routes.h"

namespace sphinx {
namespace {

ProductHttpConfig checked_config(ProductHttpConfig config) {
  validate_product_cache_policy(config.cache_policy);
  if (config.bind_address.empty() || config.port == 0 || config.worker_count == 0 ||
      config.worker_count > 64 || config.read_options.max_inflight_keys == 0 ||
      config.read_options.max_concurrent_loads == 0 ||
      config.read_options.max_concurrent_loads > config.worker_count ||
      config.read_options.wait_timeout.count() <= 0 ||
      config.read_options.wait_timeout.count() > 10000 || config.mysql.host.empty() ||
      config.mysql.port == 0 || config.mysql.user.empty() || config.mysql.database.empty() ||
      config.mysql.connect_timeout_seconds == 0 || config.mysql.read_timeout_seconds == 0 ||
      config.mysql.write_timeout_seconds == 0) {
    throw std::invalid_argument{"invalid product HTTP configuration"};
  }
  validate_product_cache_options(config.cache);
  return config;
}

struct WorkerContext final {
  WorkerContext(const ProductHttpConfig& config, ProductSharedState& shared)
      : store{config.mysql},
        cache{make_product_cache(config.cache)},
        service{store, *cache, shared, config.cache_policy} {}

  // 先初始化本线程的 MySQL 环境，再创建 store；C++ 会按成员声明的逆序析构。
  // 因此 guard 最后销毁，不会让仍在使用 MySQL 的对象失去线程环境。
  [[maybe_unused]] MySqlThreadGuard thread_guard;
  MySqlProductStore store;
  std::unique_ptr<ProductCache> cache;
  ProductService service;
};

}  // namespace

struct ProductHttpServer::Impl {
  explicit Impl(ProductHttpConfig&& source_config)
      : config{std::move(source_config)}, shared{config.read_options, config.breaker_options} {}

  ProductHttpConfig config;
  // server 及其工作队列先于 mysql_runtime 析构，确保工作线程先退出再关闭客户端库。
  [[maybe_unused]] MySqlRuntime mysql_runtime;
  ProductSharedState shared;
  httplib::Server server;
  std::mutex state_mutex;
  bool stopping = false;
  bool serve_called = false;
};

ProductHttpServer::ProductHttpServer(ProductHttpConfig config)
    : _impl{std::make_unique<Impl>(checked_config(std::move(config)))} {
  _impl->server.set_payload_max_length(65536);
  _impl->server.set_read_timeout(2, 0);
  _impl->server.set_write_timeout(2, 0);
  const auto worker_count = _impl->config.worker_count;
  // 线程池固定大小；同步 MySQL/缓存 I/O 占用 HTTP Worker，不阻塞 sphinxd 的 Reactor。
  _impl->server.new_task_queue = [worker_count] {
    return new httplib::ThreadPool{worker_count, worker_count, 256};
  };
}

ProductHttpServer::~ProductHttpServer() = default;

bool ProductHttpServer::serve() {
  {
    std::lock_guard lock{_impl->state_mutex};
    if (_impl->serve_called) {
      throw std::logic_error{"ProductHttpServer::serve may only be called once"};
    }
    _impl->serve_called = true;
    if (_impl->stopping) {
      return true;
    }
  }

  const auto* config = &_impl->config;
  auto* shared = &_impl->shared;
  // 每个 HTTP 工作线程第一次处理请求时才创建自己的数据库连接持有者和缓存客户端。
  install_product_routes(
      _impl->server,
      [config, shared]() -> ProductService& {
        thread_local std::unique_ptr<WorkerContext> context;
        if (!context) {
          context = std::make_unique<WorkerContext>(*config, *shared);
        }
        return context->service;
      },
      *shared, config->cache.backend, config->cache_policy.mode);

  {
    std::lock_guard lock{_impl->state_mutex};
    if (_impl->stopping) {
      return true;
    }
    if (!_impl->server.bind_to_port(_impl->config.bind_address, _impl->config.port)) {
      return false;
    }
  }

  const bool listen_result = _impl->server.listen_after_bind();
  std::lock_guard lock{_impl->state_mutex};
  return _impl->stopping || listen_result;
}

void ProductHttpServer::stop() noexcept {
  std::lock_guard lock{_impl->state_mutex};
  _impl->stopping = true;
  _impl->server.stop();
}

}  // namespace sphinx
