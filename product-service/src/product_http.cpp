// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_http.h>
#include <sphinx/sphinx_product_cache.h>

#include <mutex>
#include <stdexcept>
#include <utility>

#include "product_http_routes.h"

namespace sphinx {
namespace {

ProductHttpConfig checked_config(ProductHttpConfig config) {
  if (config.bind_address.empty() || config.port == 0 || config.worker_count == 0 ||
      config.worker_count > 64 || config.cache_nodes.empty() || config.cache_timeout.count() <= 0 ||
      config.cache_policy.ttl_seconds == 0 ||
      config.cache_policy.ttl_seconds > 60U * 60U * 24U * 30U || config.mysql.host.empty() ||
      config.mysql.port == 0 || config.mysql.user.empty() || config.mysql.database.empty() ||
      config.mysql.connect_timeout_seconds == 0 || config.mysql.read_timeout_seconds == 0 ||
      config.mysql.write_timeout_seconds == 0) {
    throw std::invalid_argument{"invalid product HTTP configuration"};
  }
  (void)parse_nodes(config.cache_nodes);
  return config;
}

struct WorkerContext final {
  explicit WorkerContext(const ProductHttpConfig& config)
      : thread_guard{},
        store{config.mysql},
        cache{config.cache_nodes, config.cache_timeout},
        service{store, cache, config.cache_policy} {}

  // Declaration order is mandatory: C++ destroys these members in reverse order.
  MySqlThreadGuard thread_guard;
  MySqlProductStore store;
  SphinxProductCache cache;
  ProductService service;
};

}  // namespace

struct ProductHttpServer::Impl {
  explicit Impl(ProductHttpConfig&& source_config) : config{std::move(source_config)} {}

  ProductHttpConfig config;
  // Declaration order ensures the server/worker queue is destroyed before MySQL library shutdown.
  [[maybe_unused]] MySqlRuntime mysql_runtime;
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
  _impl->server.new_task_queue = [worker_count] {
    return new httplib::ThreadPool{worker_count, worker_count, 256};
  };
}

ProductHttpServer::~ProductHttpServer() = default;

bool ProductHttpServer::serve() {
  {
    std::lock_guard<std::mutex> lock{_impl->state_mutex};
    if (_impl->serve_called) {
      throw std::logic_error{"ProductHttpServer::serve may only be called once"};
    }
    _impl->serve_called = true;
    if (_impl->stopping) {
      return true;
    }
  }

  const auto* config = &_impl->config;
  install_product_routes(_impl->server, [config]() -> ProductService& {
    thread_local std::unique_ptr<WorkerContext> context;
    if (!context) {
      context = std::make_unique<WorkerContext>(*config);
    }
    return context->service;
  });

  {
    std::lock_guard<std::mutex> lock{_impl->state_mutex};
    if (_impl->stopping) {
      return true;
    }
    if (!_impl->server.bind_to_port(_impl->config.bind_address,
                                    static_cast<int>(_impl->config.port))) {
      return false;
    }
  }

  const bool listen_result = _impl->server.listen_after_bind();
  std::lock_guard<std::mutex> lock{_impl->state_mutex};
  return _impl->stopping || listen_result;
}

void ProductHttpServer::stop() noexcept {
  std::lock_guard<std::mutex> lock{_impl->state_mutex};
  _impl->stopping = true;
  _impl->server.stop();
}

}  // namespace sphinx
