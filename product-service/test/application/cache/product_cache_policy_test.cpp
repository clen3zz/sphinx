// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/application/cache/product_cache_policy.h>

#include <stdexcept>

namespace sphinx {
namespace {

TEST(ProductCachePolicyTest, ParsesOnlySupportedPolicyModes) {
  EXPECT_EQ(parse_cache_policy_mode("basic"), CachePolicyMode::Basic);
  EXPECT_EQ(parse_cache_policy_mode("protected"), CachePolicyMode::Protected);
  EXPECT_THROW(parse_cache_policy_mode("Basic"), std::invalid_argument);
  EXPECT_THROW(parse_cache_policy_mode("other"), std::invalid_argument);
}

}  // namespace
}  // namespace sphinx
