// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_store.h>

#include <map>
#include <stdexcept>
#include <vector>

namespace sphinx {
namespace {

class RecordingStore final : public ProductStore {
 public:
  std::map<std::uint64_t, Product> rows;
  std::vector<std::uint64_t> queried_ids;

  std::optional<Product> find(std::uint64_t id) override {
    queried_ids.push_back(id);
    const auto row = rows.find(id);
    if (row == rows.end()) {
      return std::nullopt;
    }
    return row->second;
  }

  StoreUpdateResult update(const UpdateProductRequest&) override { return {}; }
};

TEST(ProductStoreTest, DefaultFindManyPreservesOrderAndDuplicates) {
  RecordingStore store;
  store.rows.emplace(1, Product{1, "tea", 199, 1});
  store.rows.emplace(2, Product{2, "coffee", 299, 1});
  const std::vector<std::uint64_t> ids{2, 99, 1, 2};

  const auto products = store.find_many(ids);

  EXPECT_EQ(store.queried_ids, ids);
  ASSERT_EQ(products.size(), 4U);
  ASSERT_TRUE(products[0]);
  EXPECT_EQ(products[0].value_or(Product{}).id, 2U);
  EXPECT_FALSE(products[1]);
  ASSERT_TRUE(products[2]);
  EXPECT_EQ(products[2].value_or(Product{}).id, 1U);
  ASSERT_TRUE(products[3]);
  EXPECT_EQ(products[3].value_or(Product{}).id, 2U);
}

TEST(ProductStoreTest, DefaultFindManyValidatesWholeBatchBeforeQuerying) {
  RecordingStore store;

  EXPECT_THROW(store.find_many({1, 0}), std::invalid_argument);
  EXPECT_TRUE(store.queried_ids.empty());

  const std::vector<std::uint64_t> oversized(max_product_batch_size + 1, 1);
  EXPECT_THROW(store.find_many(oversized), std::invalid_argument);
  EXPECT_TRUE(store.queried_ids.empty());
}

TEST(ProductStoreTest, EmptyDefaultFindManyDoesNotQuery) {
  RecordingStore store;

  EXPECT_TRUE(store.find_many({}).empty());
  EXPECT_TRUE(store.queried_ids.empty());
}

}  // namespace
}  // namespace sphinx
