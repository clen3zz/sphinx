// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace sphinx {
namespace {

ProductStatus status_from_store_error(StoreErrorCode code) noexcept {
  switch (code) {
    case StoreErrorCode::Unavailable:
      return ProductStatus::StoreUnavailable;
    case StoreErrorCode::CommitUnknown:
      return ProductStatus::CommitUnknown;
    case StoreErrorCode::InvalidData:
    case StoreErrorCode::Unexpected:
      return ProductStatus::InternalError;
  }
  return ProductStatus::InternalError;
}

}  // namespace

ProductService::ProductService(ProductStore& store, ProductCache& cache, ProductCachePolicy policy)
    : _store{store}, _cache{cache}, _policy{policy} {
  if (!valid_product_cache_ttl(_policy.ttl_seconds)) {
    throw std::invalid_argument{"product cache TTL must be in 1..30 days"};
  }
}

GetProductResult ProductService::get(std::uint64_t id, bool bypass_cache) {
  if (id == 0) {
    return {ProductStatus::InvalidArgument, std::nullopt, CacheSource::NotChecked};
  }
  const auto key = make_product_cache_key(id);
  auto source = bypass_cache ? CacheSource::Bypass : CacheSource::Miss;
  // 旁路缓存：只有命中且解码、ID 校验都通过时，才可以跳过权威数据库。
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
          // 损坏值删除失败也不影响后续数据库查询。
          // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (const CacheError&) {
          // 不让缓存故障阻断回源。
        }
      }
    } catch (const CacheError&) {
      source = CacheSource::Bypass;
    }
  }

  // 只有数据库明确返回“没有记录”才是 NotFound；连接或 SQL 失败必须单独报告。
  std::optional<Product> product;
  try {
    product = _store.find(id);
  } catch (const StoreError& error) {
    return {status_from_store_error(error.code()), std::nullopt, source};
  }
  if (!product) {
    return {ProductStatus::NotFound, std::nullopt, source};
  }
  if (!valid_product(*product) || product->id != id) {
    return {ProductStatus::InternalError, std::nullopt, source};
  }
  try {
    _cache.put(key, encode_product_cache(*product), _policy.ttl_seconds);
    // 数据库读取已成功，回填缓存失败可以忽略。
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const CacheError&) {
    // 回填只是加速后续查询，不改变本次结果。
  }
  return {ProductStatus::Ok, std::move(product), source};
}

UpdateProductResult ProductService::update(const UpdateProductRequest& request) {
  if (!valid_update_request(request)) {
    return {ProductStatus::InvalidArgument, std::nullopt, false};
  }
  // 先让数据库完成版本检查和事务提交；在结果明确前不修改缓存。
  StoreUpdateResult store_result;
  try {
    store_result = _store.update(request);
  } catch (const StoreError& error) {
    return {status_from_store_error(error.code()), std::nullopt, false};
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
  // 提交后删除缓存。删除失败只影响读到旧值的风险，不撤销已提交的数据库更新。
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
