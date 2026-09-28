// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

namespace sphinx {

/// 生成带命名空间的缓存 key；二进制格式变化时需更新 v1；要求 id > 0。
std::string make_product_cache_key(std::uint64_t id);

/// 缓存值格式：ASCII "SPC1"、三个大端 u64（id/price_cents/version）、
/// 一个大端 u16 名称字节长度，最后是对应的 UTF-8 名称字节；没有填充、校验和或尾随字节。
/// 商品数据违反约束时抛出 std::invalid_argument。
std::string encode_product_cache(const Product& product);

/// 格式错误不会抛异常；魔数错误、截断、尾随字节、UTF-8/名称无效、
/// id/version 为零或价格越界时返回 nullopt。
std::optional<Product> decode_product_cache(std::string_view bytes);

}  // 命名空间 sphinx
