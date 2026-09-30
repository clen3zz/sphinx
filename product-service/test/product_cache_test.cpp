// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_cache.h>

#include <string>

namespace sphinx {
namespace {

TEST(ProductCacheTest, EnforcesKeyLengthBounds) {
  EXPECT_FALSE(valid_product_cache_key(""));
  EXPECT_TRUE(valid_product_cache_key("product:v2:1"));
  EXPECT_TRUE(valid_product_cache_key(std::string(250, 'x')));
  EXPECT_FALSE(valid_product_cache_key(std::string(251, 'x')));
}

TEST(ProductCacheTest, RejectsWhitespaceAndControlBytes) {
  for (unsigned int value = 0; value <= 0x20U; ++value) {
    const std::string key{"p" + std::string(1, static_cast<char>(value)) + "x"};
    EXPECT_FALSE(valid_product_cache_key(key)) << value;
  }
  EXPECT_FALSE(valid_product_cache_key(std::string{"p\x7fx", 3}));
  EXPECT_TRUE(valid_product_cache_key("商品:key"));
}

}  // namespace
}  // namespace sphinx
