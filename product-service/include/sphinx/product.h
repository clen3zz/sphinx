// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sphinx {

/// 商品在数据库中的记录，也是写入缓存前的数据模型。
/// id/version 必须为正数；name 是 1..128 字节的有效 UTF-8；价格不能超过上限。
/// 跨数据库、缓存和 HTTP 边界传递时，都要保持这些约束。
struct Product {
  std::uint64_t id = 0;
  std::string name;
  std::uint64_t price_cents = 0;
  std::uint64_t version = 0;
};

inline constexpr std::uint64_t max_product_price_cents = 1'000'000'000'000ULL;
inline constexpr std::size_t max_product_name_bytes = 128;
inline constexpr std::size_t max_product_batch_size = 32;
inline constexpr std::uint32_t max_product_cache_ttl_seconds = 30U * 24U * 60U * 60U;

/// 更大的过期值会被 Memcached 文本协议解释为绝对 UNIX 时间戳。
constexpr bool valid_product_cache_ttl(std::uint32_t seconds) noexcept {
  return seconds != 0 && seconds <= max_product_cache_ttl_seconds;
}

/// 校验完整商品数据，包括 UTF-8 编码及名称中不能出现控制字符。
bool valid_product(const Product& product) noexcept;

struct UpdateProductRequest {
  std::uint64_t id = 0;
  std::string name;
  std::uint64_t price_cents = 0;
  std::uint64_t expected_version = 0;
};

/// 校验更新请求的字段和可递增的期望版本。
bool valid_update_request(const UpdateProductRequest& request) noexcept;

/// 更新成功后返回的 version 等于 expected_version + 1。
enum class ProductStatus : std::uint8_t {
  Ok,
  InvalidArgument,
  NotFound,
  Conflict,
  StoreUnavailable,
  CommitUnknown,
  InternalError,
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

enum class StoreErrorCode : std::uint8_t { Unavailable, InvalidData, CommitUnknown, Unexpected };

/// 存储层出错时抛出此异常，不能把 SQL 失败伪装成 NotFound。
class StoreError final : public std::runtime_error {
 public:
  StoreError(StoreErrorCode code, const std::string& message)
      : std::runtime_error{message}, _code{code} {}
  StoreErrorCode code() const noexcept { return _code; }

 private:
  StoreErrorCode _code;
};

class CacheError final : public std::runtime_error {
 public:
  explicit CacheError(const std::string& message) : std::runtime_error{message} {}
};

}  // namespace sphinx
