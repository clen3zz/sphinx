// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/application/ports/product_cache.h>
#include <sphinx/product/backends/redis/redis_product_cache.h>
#include <sphinx/product/backends/sphinx/sphinx_product_cache.h>
#include <sphinx/product/bootstrap/product_cache_factory.h>

#include <memory>
#include <stdexcept>

namespace sphinx {
namespace {

TEST(ProductCacheOptionsTest, ParsesOnlySupportedBackends) {
  EXPECT_EQ(parse_cache_backend("sphinx"), CacheBackend::Sphinx);
  EXPECT_EQ(parse_cache_backend("redis"), CacheBackend::Redis);
  EXPECT_THROW(parse_cache_backend("mysql"), std::invalid_argument);
  EXPECT_THROW(parse_cache_backend("Redis"), std::invalid_argument);
}

TEST(ProductCacheOptionsTest, ValidatesOnlyTheSelectedBackend) {
  ProductCacheOptions options;
  options.redis.port = 0;
  EXPECT_NO_THROW(validate_product_cache_options(options));

  options.backend = CacheBackend::Redis;
  EXPECT_THROW(validate_product_cache_options(options), std::invalid_argument);
  options.redis.port = 6379;
  options.sphinx.timeout = std::chrono::milliseconds{0};
  EXPECT_NO_THROW(validate_product_cache_options(options));
}

TEST(ProductCacheOptionsTest, CreatesTheSelectedThreadConfinedClientWithoutConnecting) {
  ProductCacheOptions options;
  std::unique_ptr<ProductCache> cache = make_product_cache(options);
  EXPECT_NE(dynamic_cast<SphinxProductCache*>(cache.get()), nullptr);

  options.backend = CacheBackend::Redis;
  options.redis.port = 1;
  cache = make_product_cache(options);
  EXPECT_NE(dynamic_cast<RedisProductCache*>(cache.get()), nullptr);
}

}  // namespace
}  // namespace sphinx
