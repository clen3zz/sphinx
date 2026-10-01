// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/application/cache/product_cache_policy.h>

#include <stdexcept>

namespace sphinx {
namespace {
constexpr std::uint32_t max_negative_cache_ttl_seconds = 30;
constexpr std::uint32_t max_product_cache_ttl_jitter_seconds = 30;
}  // namespace

void validate_product_cache_policy(const ProductCachePolicy& policy) {
  const bool valid_mode =
      policy.mode == CachePolicyMode::Basic || policy.mode == CachePolicyMode::Protected;
  if (!valid_mode || !valid_product_cache_ttl(policy.ttl_seconds) ||
      policy.negative_ttl_seconds < 1 ||
      policy.negative_ttl_seconds > max_negative_cache_ttl_seconds ||
      policy.ttl_jitter_seconds > max_product_cache_ttl_jitter_seconds) {
    throw std::invalid_argument{"invalid product cache policy"};
  }
}

CachePolicyMode parse_cache_policy_mode(std::string_view text) {
  if (text == "basic") {
    return CachePolicyMode::Basic;
  }
  if (text == "protected") {
    return CachePolicyMode::Protected;
  }
  throw std::invalid_argument{"cache policy must be basic or protected"};
}

}  // namespace sphinx
