// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/domain/product.h>

#include <optional>
#include <vector>

namespace sphinx {

/// 更新成功后返回的 version 等于 expected_version + 1。
enum class ProductStatus : std::uint8_t {
  Ok,
  InvalidArgument,
  NotFound,
  Conflict,
  StoreUnavailable,
  CommitUnknown,
  InternalError,
  ReadBusy,
};

/// Hit：缓存值解码成功；Miss：缓存中没有该 key；Bypass：缓存 I/O 失败或调用方要求直查数据库；
/// Corrupt：缓存中有值，但解码或校验失败。
enum class CacheSource : std::uint8_t { NotChecked, Hit, Miss, Bypass, Corrupt };

struct GetProductResult {
  ProductStatus status = ProductStatus::InternalError;
  std::optional<Product> product;
  CacheSource cache_source = CacheSource::NotChecked;
};

struct ProductLoadResult {
  ProductStatus status = ProductStatus::InternalError;
  std::optional<Product> product;
};

struct BatchProductItem {
  std::uint64_t id = 0;
  GetProductResult result;
};

struct GetProductsResult {
  ProductStatus status = ProductStatus::InternalError;
  std::vector<BatchProductItem> items;
};

struct UpdateProductResult {
  ProductStatus status = ProductStatus::InternalError;
  std::optional<Product> product;
  /// 仅当数据库已提交、但删除缓存失败时为 true；数据库更新仍算成功。
  bool cache_invalidation_failed = false;
};

}  // namespace sphinx
