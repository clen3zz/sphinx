// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_codec.h>

#include <stdexcept>
#include <string>

namespace {

TEST(ProductCodecTest, UsesStableBigEndianV1Frame) {
  const sphinx::Product product{1, "tea", 199, 2};
  std::string expected{"SPC1"};
  expected.append(7, '\0');
  expected.push_back('\1');
  expected.append(7, '\0');
  expected.push_back(static_cast<char>(199));
  expected.append(7, '\0');
  expected.push_back('\2');
  expected.push_back('\0');
  expected.push_back('\3');
  expected += "tea";
  EXPECT_EQ(sphinx::encode_product_cache(product), expected);
  const auto decoded = sphinx::decode_product_cache(expected);
  if (!decoded) {
    ADD_FAILURE() << "valid cache frame did not decode";
    return;
  }
  EXPECT_EQ(decoded->id, product.id);
  EXPECT_EQ(decoded->name, product.name);
  EXPECT_EQ(decoded->price_cents, product.price_cents);
  EXPECT_EQ(decoded->version, product.version);
}

TEST(ProductCodecTest, RejectsTruncatedTrailingAndWrongVersionFrames) {
  const auto frame = sphinx::encode_product_cache({7, "x", 0, 1});
  EXPECT_FALSE(sphinx::decode_product_cache(frame.substr(0, frame.size() - 1)));
  EXPECT_FALSE(sphinx::decode_product_cache(frame + "x"));
  auto changed = frame;
  changed[3] = '2';
  EXPECT_FALSE(sphinx::decode_product_cache(changed));
  changed = frame;
  changed[29] = '\2';
  EXPECT_FALSE(sphinx::decode_product_cache(changed));
}

TEST(ProductCodecTest, RejectsInvalidDomainAndUtf8) {
  EXPECT_THROW(sphinx::encode_product_cache({0, "x", 0, 1}), std::invalid_argument);
  EXPECT_THROW(sphinx::encode_product_cache({1, "", 0, 1}), std::invalid_argument);
  EXPECT_THROW(sphinx::encode_product_cache({1, "x", 0, 0}), std::invalid_argument);
  EXPECT_THROW(sphinx::encode_product_cache({1, "x", sphinx::max_product_price_cents + 1, 1}),
               std::invalid_argument);
  EXPECT_THROW(sphinx::encode_product_cache({1, std::string{static_cast<char>(0xC0)}, 0, 1}),
               std::invalid_argument);
  EXPECT_FALSE(sphinx::valid_product({1, "line\nfeed", 0, 1}));
}

TEST(ProductCodecTest, SupportsUtf8AndMaximumNameLength) {
  const sphinx::Product product{42, std::string(128, 'x'), 0, 1};
  EXPECT_TRUE(sphinx::decode_product_cache(sphinx::encode_product_cache(product)));
  EXPECT_FALSE(sphinx::valid_product({42, std::string(129, 'x'), 0, 1}));
  const sphinx::Product chinese{43, "咖啡", 500, 1};
  EXPECT_TRUE(sphinx::decode_product_cache(sphinx::encode_product_cache(chinese)));
}

TEST(ProductCodecTest, KeyIsNamespacedAndRejectsZero) {
  EXPECT_EQ(sphinx::make_product_cache_key(42), "product:v1:42");
  EXPECT_THROW(sphinx::make_product_cache_key(0), std::invalid_argument);
}

}  // namespace
