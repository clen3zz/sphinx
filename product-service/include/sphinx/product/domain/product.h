// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

}  // namespace sphinx
