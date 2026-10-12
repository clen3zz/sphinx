// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/backends/mysql/mysql_product_store.h>
#include <sphinx/product/backends/mysql/mysql_runtime.h>
#include <sphinx/product/bootstrap/product_worker.h>

#include <memory>

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

}  // namespace

void run_product_worker(const ProductRuntimeConfig& config, ProductSharedState& shared,
                        const ProductWorkerLoop& run_jobs) {
  WorkerContext worker{config, shared};
  run_jobs(worker.service);
}

}  // namespace sphinx
