// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/application/product_limits.h>

#include <cstdint>
#include <string_view>

namespace sphinx {

enum class CachePolicyMode : std::uint8_t { Basic, Protected };

struct ProductCachePolicy {
  CachePolicyMode mode = CachePolicyMode::Basic;
  /// 必须在 1..2,592,000 秒内；更大的过期值会被 Sphinx 解释为 UNIX 时间戳。
  std::uint32_t ttl_seconds = 30;
  std::uint32_t negative_ttl_seconds = 5;
  std::uint32_t ttl_jitter_seconds = 3;
};

/// Validates policy bounds before starting workers or constructing a service.
void validate_product_cache_policy(const ProductCachePolicy& policy);

CachePolicyMode parse_cache_policy_mode(std::string_view text);

}  // namespace sphinx
