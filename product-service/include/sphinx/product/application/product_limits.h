// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace sphinx {

inline constexpr std::size_t max_product_batch_size = 32;
inline constexpr std::uint32_t max_product_cache_ttl_seconds = 30U * 24U * 60U * 60U;

/// 更大的过期值会被 Memcached 文本协议解释为绝对 UNIX 时间戳。
constexpr bool valid_product_cache_ttl(std::uint32_t seconds) noexcept {
  return seconds != 0 && seconds <= max_product_cache_ttl_seconds;
}

}  // namespace sphinx
