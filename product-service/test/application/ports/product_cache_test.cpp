// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/application/ports/product_cache.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace sphinx {
namespace {

class RecordingCache final : public ProductCache {
 public:
  std::vector<std::string> read_keys;
  std::vector<CacheWriteEntry> writes;
  bool fail_on_second_write = false;

  std::optional<std::string> get(std::string_view key) override {
    read_keys.emplace_back(key);
    if (key == "missing") {
      return std::nullopt;
    }
    return std::string{key} + "-value";
  }

  void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) override {
    writes.push_back({std::string{key}, std::string{value}, ttl_seconds});
    if (fail_on_second_write && writes.size() == 2) {
      throw CacheError{"simulated partial batch failure"};
    }
  }

  void erase(std::string_view) override {}
};

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

TEST(ProductCacheTest, DefaultGetManyPreservesOrderAndDuplicates) {
  RecordingCache cache;
  const std::vector<std::string> keys{"second", "missing", "second"};

  const auto values = cache.get_many(keys);

  EXPECT_EQ(cache.read_keys, keys);
  ASSERT_EQ(values.size(), 3U);
  ASSERT_TRUE(values[0]);
  EXPECT_EQ(values[0].value_or(""), "second-value");
  EXPECT_FALSE(values[1]);
  ASSERT_TRUE(values[2]);
  EXPECT_EQ(values[2].value_or(""), "second-value");
}

TEST(ProductCacheTest, DefaultGetManyValidatesWholeBatchBeforeReading) {
  RecordingCache cache;

  EXPECT_THROW(cache.get_many({"valid", "bad key"}), std::invalid_argument);
  EXPECT_TRUE(cache.read_keys.empty());

  std::vector<std::string> oversized(max_product_batch_size + 1, "valid");
  EXPECT_THROW(cache.get_many(oversized), std::invalid_argument);
  EXPECT_TRUE(cache.read_keys.empty());
}

TEST(ProductCacheTest, DefaultPutManyValidatesWholeBatchBeforeWriting) {
  RecordingCache cache;
  const std::vector<CacheWriteEntry> entries{{"valid", "value", 10}, {"later", "value", 0}};

  EXPECT_THROW(cache.put_many(entries), std::invalid_argument);

  EXPECT_TRUE(cache.writes.empty());
}

TEST(ProductCacheTest, DefaultPutManyCanFailAfterEarlierWrites) {
  RecordingCache cache;
  cache.fail_on_second_write = true;
  const std::vector<CacheWriteEntry> entries{{"first", "one", 10}, {"second", "two", 20}};

  EXPECT_THROW(cache.put_many(entries), CacheError);

  ASSERT_EQ(cache.writes.size(), 2U);
  EXPECT_EQ(cache.writes[0].key, "first");
  EXPECT_EQ(cache.writes[1].key, "second");
}

TEST(ProductCacheTest, EmptyDefaultBatchesDoNotCallSingleItemOperations) {
  RecordingCache cache;

  EXPECT_TRUE(cache.get_many({}).empty());
  cache.put_many({});

  EXPECT_TRUE(cache.read_keys.empty());
  EXPECT_TRUE(cache.writes.empty());
}

}  // namespace
}  // namespace sphinx
