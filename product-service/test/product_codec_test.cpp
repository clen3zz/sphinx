// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace {

TEST(ProductCodecTest, EncodesReadableJson) {
  const sphinx::Product product{1, "tea", 199, 2};
  const std::string expected = R"({"id":1,"name":"tea","price_cents":199,"version":2})";
  EXPECT_EQ(sphinx::encode_product_cache(product), expected);
  const auto decoded = sphinx::decode_product_cache(expected);
  if (!decoded) {
    ADD_FAILURE() << "valid cache JSON did not decode";
    return;
  }
  EXPECT_EQ(decoded->id, product.id);
  EXPECT_EQ(decoded->name, product.name);
  EXPECT_EQ(decoded->price_cents, product.price_cents);
  EXPECT_EQ(decoded->version, product.version);
}

TEST(ProductCodecTest, RejectsMalformedAndUnexpectedFields) {
  EXPECT_FALSE(sphinx::decode_product_cache(R"({"id":7)"));
  EXPECT_FALSE(
      sphinx::decode_product_cache(R"({"id":7,"name":"x","price_cents":0,"version":1,"extra":0})"));
  EXPECT_FALSE(sphinx::decode_product_cache(R"({"id":7,"name":"x","price_cents":-1,"version":1})"));
  EXPECT_FALSE(sphinx::decode_product_cache(std::string(513, 'x')));
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
  EXPECT_EQ(sphinx::make_product_cache_key(42), "product:v3:42");
  EXPECT_THROW(sphinx::make_product_cache_key(0), std::invalid_argument);
}

TEST(ProductCodecTest, EncodesAndDecodesMatchingNegativeCacheEntries) {
  const auto payload = sphinx::encode_product_not_found(42);
  EXPECT_EQ(payload, R"({"id":42,"not_found":true})");
  const auto decoded = sphinx::decode_product_cache_entry(payload, 42);
  EXPECT_EQ(decoded.kind, sphinx::CacheEntryKind::NotFound);
  EXPECT_FALSE(decoded.product);
  EXPECT_THROW(sphinx::encode_product_not_found(0), std::invalid_argument);
}

TEST(ProductCodecTest, RejectsMalformedNegativeCacheEntries) {
  EXPECT_EQ(sphinx::decode_product_cache_entry(R"({"id":42,"not_found":false})", 42).kind,
            sphinx::CacheEntryKind::Corrupt);
  EXPECT_EQ(sphinx::decode_product_cache_entry(R"({"id":42,"not_found":1})", 42).kind,
            sphinx::CacheEntryKind::Corrupt);
  EXPECT_EQ(sphinx::decode_product_cache_entry(R"({"id":42,"not_found":true,"name":"x"})", 42).kind,
            sphinx::CacheEntryKind::Corrupt);
  EXPECT_EQ(sphinx::decode_product_cache_entry(R"({"id":42})", 42).kind,
            sphinx::CacheEntryKind::Corrupt);
  EXPECT_EQ(sphinx::decode_product_cache_entry(R"({"id":43,"not_found":true})", 42).kind,
            sphinx::CacheEntryKind::Corrupt);
  EXPECT_EQ(sphinx::decode_product_cache_entry(R"({"id":42,"not_found":true)", 42).kind,
            sphinx::CacheEntryKind::Corrupt);
}

TEST(ProductCodecTest, DecodesOnlyMatchingPositiveCacheEntries) {
  const auto payload = sphinx::encode_product_cache({42, "tea", 199, 2});
  const auto decoded = sphinx::decode_product_cache_entry(payload, 42);
  ASSERT_EQ(decoded.kind, sphinx::CacheEntryKind::Product);
  ASSERT_TRUE(decoded.product.has_value());
  const auto product = decoded.product.value_or(sphinx::Product{});
  EXPECT_EQ(product.id, 42);
  EXPECT_EQ(sphinx::decode_product_cache_entry(payload, 43).kind, sphinx::CacheEntryKind::Corrupt);
  EXPECT_EQ(sphinx::decode_product_cache_entry(
                R"({"id":42,"name":"tea","price_cents":199,"version":2,"extra":0})", 42)
                .kind,
            sphinx::CacheEntryKind::Corrupt);
}

TEST(ProductCodecTest, ProductCacheTtlIsStableAndBounded) {
  sphinx::ProductCachePolicy basic;
  basic.ttl_seconds = 30;
  EXPECT_EQ(sphinx::product_cache_ttl(42, basic), 30U);

  sphinx::ProductCachePolicy protected_policy;
  protected_policy.mode = sphinx::CachePolicyMode::Protected;
  protected_policy.ttl_seconds = 30;
  protected_policy.ttl_jitter_seconds = 3;
  const auto first = sphinx::product_cache_ttl(42, protected_policy);
  EXPECT_EQ(sphinx::product_cache_ttl(42, protected_policy), first);
  EXPECT_GE(first, 27U);
  EXPECT_LE(first, 33U);

  protected_policy.ttl_seconds = 1;
  EXPECT_GE(sphinx::product_cache_ttl(1, protected_policy), 1U);
  protected_policy.ttl_seconds = sphinx::max_product_cache_ttl_seconds;
  EXPECT_LE(sphinx::product_cache_ttl(1, protected_policy), sphinx::max_product_cache_ttl_seconds);
}

TEST(ProductRulesTest, UpdateVersionAndCacheTtlBoundaries) {
  const sphinx::UpdateProductRequest request{1, "tea", 199, 1};
  EXPECT_TRUE(sphinx::valid_update_request(request));
  EXPECT_FALSE(sphinx::valid_update_request({1, "tea", 199, 0}));
  EXPECT_FALSE(
      sphinx::valid_update_request({1, "tea", 199, std::numeric_limits<std::uint64_t>::max()}));
  EXPECT_FALSE(sphinx::valid_product_cache_ttl(0));
  EXPECT_TRUE(sphinx::valid_product_cache_ttl(sphinx::max_product_cache_ttl_seconds));
  EXPECT_FALSE(sphinx::valid_product_cache_ttl(sphinx::max_product_cache_ttl_seconds + 1));
}

}  // namespace
