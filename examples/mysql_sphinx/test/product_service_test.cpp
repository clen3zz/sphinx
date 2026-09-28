// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

#include <optional>
#include <string>

namespace {

class FakeStore final : public sphinx::ProductStore {
 public:
  std::optional<sphinx::Product> row;
  sphinx::StoreUpdateResult next_update{};
  std::optional<sphinx::StoreErrorCode> find_error;
  std::optional<sphinx::StoreErrorCode> update_error;
  int finds = 0;
  int updates = 0;

  std::optional<sphinx::Product> find(std::uint64_t) override {
    ++finds;
    if (find_error) {
      throw sphinx::StoreError{*find_error, "fake store failure"};
    }
    return row;
  }
  sphinx::StoreUpdateResult update(const sphinx::UpdateProductRequest&) override {
    ++updates;
    if (update_error) {
      throw sphinx::StoreError{*update_error, "fake store failure"};
    }
    return next_update;
  }
};

class FakeCache final : public sphinx::ProductCache {
 public:
  std::optional<std::string> value;
  bool fail_get = false;
  bool fail_put = false;
  bool fail_erase = false;
  int gets = 0;
  int puts = 0;
  int erases = 0;
  std::uint32_t last_ttl = 0;

  std::optional<std::string> get(std::string_view) override {
    ++gets;
    if (fail_get) {
      throw sphinx::CacheError{"cache unavailable"};
    }
    return value;
  }
  void put(std::string_view, std::string_view bytes, std::uint32_t ttl_seconds) override {
    ++puts;
    if (fail_put) {
      throw sphinx::CacheError{"cache unavailable"};
    }
    value = std::string{bytes};
    last_ttl = ttl_seconds;
  }
  void erase(std::string_view) override {
    ++erases;
    if (fail_erase) {
      throw sphinx::CacheError{"cache unavailable"};
    }
    value.reset();
  }
};

TEST(ProductServiceTest, HitDoesNotReadDatabase) {
  FakeStore store;
  FakeCache cache;
  cache.value = sphinx::encode_product_cache({1, "tea", 199, 1});
  sphinx::ProductService service{store, cache};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, sphinx::CacheSource::Hit);
  ASSERT_TRUE(result.product);
  EXPECT_EQ(result.product->name, "tea");
  EXPECT_EQ(store.finds, 0);
}

TEST(ProductServiceTest, MissReadsDatabaseAndFillsWithRelativeTtl) {
  FakeStore store;
  store.row = sphinx::Product{1, "tea", 199, 1};
  FakeCache cache;
  sphinx::ProductService service{store, cache, {45}};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, sphinx::CacheSource::Miss);
  EXPECT_EQ(store.finds, 1);
  EXPECT_EQ(cache.puts, 1);
  EXPECT_EQ(cache.last_ttl, 45U);
  ASSERT_TRUE(cache.value);
  EXPECT_EQ(sphinx::decode_product_cache(*cache.value)->version, 1U);
}

TEST(ProductServiceTest, CacheFailureBypassesAndDatabaseFailureIsUnavailable) {
  FakeStore store;
  store.find_error = sphinx::StoreErrorCode::Unavailable;
  FakeCache cache;
  cache.fail_get = true;
  sphinx::ProductService service{store, cache};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, sphinx::ProductStatus::StoreUnavailable);
  EXPECT_EQ(result.cache_source, sphinx::CacheSource::Bypass);
  EXPECT_EQ(store.finds, 1);
}

TEST(ProductServiceTest, CorruptOrWrongIdCacheValueIsErasedAndReplaced) {
  FakeStore store;
  store.row = sphinx::Product{1, "fresh", 300, 2};
  FakeCache cache;
  cache.value = sphinx::encode_product_cache({2, "wrong", 100, 1});
  sphinx::ProductService service{store, cache};
  const auto result = service.get(1);
  EXPECT_EQ(result.cache_source, sphinx::CacheSource::Corrupt);
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_EQ(cache.erases, 1);
  ASSERT_TRUE(result.product);
  EXPECT_EQ(result.product->version, 2U);
}

TEST(ProductServiceTest, MissingRowIsNotNegativelyCached) {
  FakeStore store;
  FakeCache cache;
  sphinx::ProductService service{store, cache};
  EXPECT_EQ(service.get(1).status, sphinx::ProductStatus::NotFound);
  EXPECT_EQ(cache.puts, 0);
}

TEST(ProductServiceTest, FreshReadBypassesStaleCacheForCommitReconciliation) {
  FakeStore store;
  store.row = sphinx::Product{1, "new", 300, 2};
  FakeCache cache;
  cache.value = sphinx::encode_product_cache({1, "old", 200, 1});
  sphinx::ProductService service{store, cache};
  const auto result = service.get(1, true);
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, sphinx::CacheSource::Bypass);
  ASSERT_TRUE(result.product);
  EXPECT_EQ(result.product->version, 2U);
  EXPECT_EQ(cache.gets, 0);
  EXPECT_EQ(store.finds, 1);
}

TEST(ProductServiceTest, SuccessfulUpdateCommitsBeforeInvalidation) {
  FakeStore store;
  store.next_update = {sphinx::StoreUpdateStatus::Updated, sphinx::Product{1, "new", 250, 2}};
  FakeCache cache;
  cache.value = sphinx::encode_product_cache({1, "old", 200, 1});
  sphinx::ProductService service{store, cache};
  const auto result = service.update({1, "new", 250, 1});
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_FALSE(result.cache_invalidation_failed);
  EXPECT_EQ(store.updates, 1);
  EXPECT_EQ(cache.erases, 1);
  EXPECT_FALSE(cache.value);
}

TEST(ProductServiceTest, CommittedUpdateSurvivesCacheFailure) {
  FakeStore store;
  store.next_update = {sphinx::StoreUpdateStatus::Updated, sphinx::Product{1, "new", 250, 2}};
  FakeCache cache;
  cache.fail_erase = true;
  sphinx::ProductService service{store, cache};
  const auto result = service.update({1, "new", 250, 1});
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_TRUE(result.cache_invalidation_failed);
}

TEST(ProductServiceTest, ConflictAndUnknownCommitDoNotTouchCache) {
  FakeStore store;
  FakeCache cache;
  sphinx::ProductService service{store, cache};
  store.next_update = {sphinx::StoreUpdateStatus::Conflict, std::nullopt};
  EXPECT_EQ(service.update({1, "new", 250, 1}).status, sphinx::ProductStatus::Conflict);
  store.update_error = sphinx::StoreErrorCode::CommitUnknown;
  EXPECT_EQ(service.update({1, "new", 250, 1}).status, sphinx::ProductStatus::CommitUnknown);
  EXPECT_EQ(cache.erases, 0);
  EXPECT_EQ(store.updates, 2);
}

TEST(ProductServiceTest, InvalidInputDoesNotUseDependencies) {
  FakeStore store;
  FakeCache cache;
  sphinx::ProductService service{store, cache};
  EXPECT_EQ(service.get(0).status, sphinx::ProductStatus::InvalidArgument);
  EXPECT_EQ(service.update({1, "", 1, 1}).status, sphinx::ProductStatus::InvalidArgument);
  EXPECT_EQ(store.finds, 0);
  EXPECT_EQ(store.updates, 0);
  EXPECT_EQ(cache.gets, 0);
}

TEST(ProductServiceTest, ConcurrentStaleFillExpiresByOwnTtl) {
  // TODO(agent): Add a deterministic two-thread barrier test using two independent services and
  // per-thread fake store/cache handles over shared synchronized fake state. Pause old GET after
  // DB read, commit PUT and erase, resume GET fill, then advance fake clock to TTL expiry. Assert
  // old value may be returned before its own expiry and the next miss loads the committed version.
  GTEST_SKIP() << "deterministic race harness belongs to the concurrency validation task";
}

}  // namespace
