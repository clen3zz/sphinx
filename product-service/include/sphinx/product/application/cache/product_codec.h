// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/domain/product.h>

#include <optional>

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

/// Encodes a strict negative-cache marker for a positive product ID.
std::string encode_product_not_found(std::uint64_t id);

/// 解码并校验商品或负缓存标记及请求 ID；格式或领域约束无效时返回 Corrupt。
DecodedProductCacheEntry decode_product_cache_entry(std::string_view payload,
                                                    std::uint64_t expected_id);

/// Applies deterministic positive-cache TTL jitter only in protected mode.
std::uint32_t product_cache_ttl(std::uint64_t id, const ProductCachePolicy& policy) noexcept;

}  // namespace sphinx
