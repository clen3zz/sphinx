// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

namespace sphinx {

/// 生成带命名空间的缓存 key；缓存格式变化时需更新版本前缀；要求 id > 0。
std::string make_product_cache_key(std::uint64_t id);

/// 缓存值是包含 id、name、price_cents、version 的 JSON 对象。
/// 商品数据违反约束时抛出 std::invalid_argument。
std::string encode_product_cache(const Product& product);

/// 格式错误不会抛异常；字段缺失、类型错误、UTF-8/名称无效、
/// id/version 为零或价格越界时返回 nullopt。
std::optional<Product> decode_product_cache(std::string_view bytes);

}  // 命名空间 sphinx
