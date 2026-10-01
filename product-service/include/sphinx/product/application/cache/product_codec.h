// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/domain/product.h>

namespace sphinx {

struct ProductCachePolicy;

enum class CacheEntryKind : std::uint8_t { Product, NotFound, Corrupt };

struct DecodedProductCacheEntry {
  CacheEntryKind kind = CacheEntryKind::Corrupt;
  std::optional<Product> product;
};

/// 生成带命名空间的缓存 key；缓存格式变化时需更新版本前缀；要求 id > 0。
std::string make_product_cache_key(std::uint64_t id);

/// 缓存值是包含 id、name、price_cents、version 的 JSON 对象。
/// 商品数据违反约束时抛出 std::invalid_argument。
std::string encode_product_cache(const Product& product);

/// 格式错误不会抛异常；字段缺失、类型错误、UTF-8/名称无效、
/// id/version 为零或价格越界时返回 nullopt。
std::optional<Product> decode_product_cache(std::string_view bytes);

/// Encodes a strict negative-cache marker for a positive product ID.
std::string encode_product_not_found(std::uint64_t id);

/// Decodes either a positive product or a matching negative-cache marker.
DecodedProductCacheEntry decode_product_cache_entry(std::string_view payload,
                                                    std::uint64_t expected_id);

/// Applies deterministic positive-cache TTL jitter only in protected mode.
std::uint32_t product_cache_ttl(std::uint64_t id, const ProductCachePolicy& policy) noexcept;

}  // namespace sphinx
