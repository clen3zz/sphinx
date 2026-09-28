// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_cache.h>
#include <sphinx/product_store.h>

namespace sphinx {

struct ProductCachePolicy {
  /// Must be in 1..2,592,000, because Sphinx treats larger expirations as UNIX timestamps.
  std::uint32_t ttl_seconds = 30;
};

/// Cache-aside application service. Store and cache are borrowed and must outlive this object.
/// All three objects are confined to a single worker thread. No lock is taken in this layer.
class ProductService final {
 public:
  ProductService(ProductStore& store, ProductCache& cache, ProductCachePolicy policy = {});

  /// GET: cache -> decode/validate id -> database on miss/error -> best-effort fill. With
  /// bypass_cache=true, query the primary database directly (used for commit reconciliation).
  /// No negative caching. A cache hit can be stale until TTL expiry.
  GetProductResult get(std::uint64_t id, bool bypass_cache = false);

  /// PUT: database transaction and COMMIT -> best-effort cache erase. No cache change before
  /// commit. Optimistic version conflict is not an error. CommitUnknown must not trigger
  /// deletion/retry.
  UpdateProductResult update(const UpdateProductRequest& request);

 private:
  ProductStore& _store;
  ProductCache& _cache;
  ProductCachePolicy _policy;
};

}  // namespace sphinx
