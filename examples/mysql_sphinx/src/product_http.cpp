// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_http.h>
#include <sphinx/sphinx_product_cache.h>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "product_http_routes.h"

// TODO(agent): Build the fixed worker queue described in IMPLEMENTATION_PLAN.md; never share
// ProductService or its connection-owning dependencies between HTTP workers.

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
  explicit Impl(ProductHttpConfig source_config) : config{std::move(source_config)} {}

  ProductHttpConfig config;
  // Declaration order ensures the server/worker queue is destroyed before MySQL library shutdown.
  MySqlRuntime mysql_runtime;
  httplib::Server server;
  std::mutex state_mutex;
  std::atomic<bool> stopping{false};
};

ProductHttpServer::ProductHttpServer(ProductHttpConfig config)
    : _impl{std::make_unique<Impl>(checked_config(std::move(config)))} {
  // TODO(agent): Configure server max payload 64 KiB, 2-second read/write timeouts, and the
  // bounded fixed worker queue. MySqlRuntime is already initialized in Impl before workers.
}

ProductHttpServer::~ProductHttpServer() = default;

bool ProductHttpServer::serve() {
  // TODO(agent): Call install_product_routes(server, current_service) from product_http_routes.h;
  // current_service lazily creates a thread_local WorkerContext with its own guard, store, cache
  // and service. Set server.new_task_queue to a bounded fixed httplib::ThreadPool(n, n, 256).
  // Under state_mutex, check stopping then call server.bind_to_port; unlock before
  // server.listen_after_bind() blocks. Return false on actual bind/listen failure, true on a
  // requested stop. This ordering closes the pre-listen stop race. No shared MYSQL/socket handle.
  return false;
}

void ProductHttpServer::stop() noexcept {
  // TODO(agent): Under state_mutex set stopping=true, then call server.stop() even if serve() has
  // only bound its socket. This prevents a stop-before-bind race. serve() joins workers before it
  // returns. Make repeated calls safe. Never allocate or throw in this operation.
}

}  // namespace sphinx
