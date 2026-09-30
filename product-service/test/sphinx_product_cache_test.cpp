// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/sphinx_product_cache.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace sphinx {
namespace {

TEST(SphinxProductCacheTest, EmptyBatchesReturnWithoutNetworkAccess) {
  SphinxProductCache cache{"127.0.0.1:1"};

  EXPECT_TRUE(cache.get_many({}).empty());
  EXPECT_NO_THROW(cache.put_many({}));
}

TEST(SphinxProductCacheTest, GetManyValidatesEveryKeyBeforeNetworkAccess) {
  SphinxProductCache cache{"127.0.0.1:1"};

  EXPECT_THROW(cache.get_many({"valid", "bad key"}), std::invalid_argument);
  const std::vector<std::string> oversized_keys(max_product_batch_size + 1, "valid");
  EXPECT_THROW(cache.get_many(oversized_keys), std::invalid_argument);
}

TEST(SphinxProductCacheTest, PutManyValidatesEveryEntryBeforeTheFirstWrite) {
  SphinxProductCache cache{"127.0.0.1:1"};
  const std::vector<CacheWriteEntry> invalid_ttl{{"valid", "first", 10}, {"later", "invalid", 0}};
  const std::vector<CacheWriteEntry> invalid_key{{"valid", "first", 10},
                                                 {"bad key", "invalid", 10}};

  EXPECT_THROW(cache.put_many(invalid_ttl), std::invalid_argument);
  EXPECT_THROW(cache.put_many(invalid_key), std::invalid_argument);

  std::vector<CacheWriteEntry> oversized(max_product_batch_size + 1, {"valid", "value", 10});
  EXPECT_THROW(cache.put_many(oversized), std::invalid_argument);
}

}  // namespace
}  // namespace sphinx
