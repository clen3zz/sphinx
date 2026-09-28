// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sphinx {

ProductService::ProductService(ProductStore& store, ProductCache& cache, ProductCachePolicy policy)
    : _store{store}, _cache{cache}, _policy{policy} {
  if (_policy.ttl_seconds == 0 || _policy.ttl_seconds > 60U * 60U * 24U * 30U) {
    throw std::invalid_argument{"product cache TTL must be in 1..30 days"};
  }
}

GetProductResult ProductService::get(std::uint64_t id, bool bypass_cache) {
  if (id == 0) {
    return {ProductStatus::InvalidArgument, std::nullopt, CacheSource::NotChecked};
  }
  const auto key = make_product_cache_key(id);
  auto source = bypass_cache ? CacheSource::Bypass : CacheSource::Miss;
  if (!bypass_cache) {
    try {
      const auto cached = _cache.get(key);
      if (cached) {
        auto product = decode_product_cache(*cached);
        if (product && product->id == id) {
          return {ProductStatus::Ok, std::move(product), CacheSource::Hit};
        }
        source = CacheSource::Corrupt;
        try {
          _cache.erase(key);
          // Cache invalidation after a corrupt entry is optional for the database read.
          // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (const CacheError&) {
          // A corrupt cache entry never prevents a database read.
        }
      }
    } catch (const CacheError&) {
      source = CacheSource::Bypass;
    }
  }

  std::optional<Product> product;
  try {
    product = _store.find(id);
  } catch (const StoreError& error) {
    switch (error.code()) {
      case StoreErrorCode::Unavailable:
        return {ProductStatus::StoreUnavailable, std::nullopt, source};
      case StoreErrorCode::CommitUnknown:
        return {ProductStatus::CommitUnknown, std::nullopt, source};
      case StoreErrorCode::InvalidData:
      case StoreErrorCode::Unexpected:
        return {ProductStatus::InternalError, std::nullopt, source};
    }
    return {ProductStatus::InternalError, std::nullopt, source};
  }
  if (!product) {
    return {ProductStatus::NotFound, std::nullopt, source};
  }
  if (!valid_product(*product) || product->id != id) {
    return {ProductStatus::InternalError, std::nullopt, source};
  }
  try {
    _cache.put(key, encode_product_cache(*product), _policy.ttl_seconds);
    // The authoritative database read has succeeded; a cache fill may be skipped.
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const CacheError&) {
    // A cache fill is best effort after the authoritative read.
  }
  return {ProductStatus::Ok, std::move(product), source};
}

UpdateProductResult ProductService::update(const UpdateProductRequest& request) {
  if (request.id == 0 || request.expected_version == 0 ||
      request.expected_version == std::numeric_limits<std::uint64_t>::max() ||
      !valid_product(Product{request.id, request.name, request.price_cents, 1})) {
    return {ProductStatus::InvalidArgument, std::nullopt, false};
  }
  StoreUpdateResult store_result;
  try {
    store_result = _store.update(request);
  } catch (const StoreError& error) {
    switch (error.code()) {
      case StoreErrorCode::Unavailable:
        return {ProductStatus::StoreUnavailable, std::nullopt, false};
      case StoreErrorCode::CommitUnknown:
        return {ProductStatus::CommitUnknown, std::nullopt, false};
      case StoreErrorCode::InvalidData:
      case StoreErrorCode::Unexpected:
        return {ProductStatus::InternalError, std::nullopt, false};
    }
    return {ProductStatus::InternalError, std::nullopt, false};
  }
  if (store_result.status == StoreUpdateStatus::NotFound) {
    return {ProductStatus::NotFound, std::nullopt, false};
  }
  if (store_result.status == StoreUpdateStatus::Conflict) {
    return {ProductStatus::Conflict, std::nullopt, false};
  }
  if (store_result.status != StoreUpdateStatus::Updated) {
    return {ProductStatus::InternalError, std::nullopt, false};
  }
  bool invalidation_failed = false;
  try {
    _cache.erase(make_product_cache_key(request.id));
  } catch (const CacheError&) {
    invalidation_failed = true;
  }
  if (!store_result.product || !valid_product(*store_result.product) ||
      store_result.product->id != request.id ||
      store_result.product->version != request.expected_version + 1) {
    return {ProductStatus::InternalError, std::nullopt, invalidation_failed};
  }
  return {ProductStatus::Ok, std::move(store_result.product), invalidation_failed};
}

}  // namespace sphinx
