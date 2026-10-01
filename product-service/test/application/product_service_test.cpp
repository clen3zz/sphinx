// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/application/cache/product_codec.h>
#include <sphinx/product/application/product_service.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

class FakeStore final : public ProductStore {
 public:
  std::optional<Product> row;
  StoreUpdateResult next_update{};
  std::optional<StoreErrorCode> find_error;
  std::optional<StoreErrorCode> update_error;
  int finds = 0;
  int updates = 0;

  std::optional<Product> find(std::uint64_t) override {
    ++finds;
    if (find_error) {
      throw StoreError{*find_error, "fake store failure"};
    }
    return row;
  }
  StoreUpdateResult update(const UpdateProductRequest&) override {
    ++updates;
    if (update_error) {
      throw StoreError{*update_error, "fake store failure"};
    }
    return next_update;
  }
};

class FakeCache final : public ProductCache {
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
      throw CacheError{"cache unavailable"};
    }
    return value;
  }
  void put(std::string_view, std::string_view bytes, std::uint32_t ttl_seconds) override {
    ++puts;
    if (fail_put) {
      throw CacheError{"cache unavailable"};
    }
    value = std::string{bytes};
    last_ttl = ttl_seconds;
  }
  void erase(std::string_view) override {
    ++erases;
    if (fail_erase) {
      throw CacheError{"cache unavailable"};
    }
    value.reset();
  }
};

class BatchStore final : public ProductStore {
 public:
  std::map<std::uint64_t, Product> rows;
  std::vector<std::vector<std::uint64_t>> batch_queries;
  std::optional<StoreErrorCode> batch_error;
  int finds = 0;

  std::optional<Product> find(std::uint64_t id) override {
    ++finds;
    const auto product = rows.find(id);
    if (product == rows.end()) {
      return std::nullopt;
    }
    return product->second;
  }

  std::vector<std::optional<Product>> find_many(const std::vector<std::uint64_t>& ids) override {
    batch_queries.push_back(ids);
    if (batch_error) {
      throw StoreError{*batch_error, "simulated batch store failure"};
    }
    std::vector<std::optional<Product>> products;
    products.reserve(ids.size());
    for (const auto id : ids) {
      const auto product = rows.find(id);
      if (product == rows.end()) {
        products.emplace_back(std::nullopt);
      } else {
        products.emplace_back(product->second);
      }
    }
    return products;
  }

  StoreUpdateResult update(const UpdateProductRequest&) override { return {}; }
};

class BatchCache final : public ProductCache {
 public:
  std::map<std::string, std::string> values;
  std::vector<std::vector<std::string>> batch_reads;
  std::vector<CacheWriteEntry> batch_writes;
  bool fail_batch_read = false;

  std::optional<std::string> get(std::string_view key) override {
    const auto value = values.find(std::string{key});
    if (value == values.end()) {
      return std::nullopt;
    }
    return value->second;
  }

  void put(std::string_view key, std::string_view value, std::uint32_t) override {
    values[std::string{key}] = std::string{value};
  }

  void erase(std::string_view key) override { values.erase(std::string{key}); }

  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) override {
    batch_reads.push_back(keys);
    if (fail_batch_read) {
      throw CacheError{"simulated batch cache failure"};
    }
    return ProductCache::get_many(keys);
  }

  void put_many(const std::vector<CacheWriteEntry>& entries) override {
    batch_writes.insert(batch_writes.end(), entries.begin(), entries.end());
    for (const auto& entry : entries) {
      values[entry.key] = entry.value;
    }
  }
};

// 共享依赖只负责搭建单条/批量读取环境；每个测试仍明确设置输入与断言。
struct ProductServiceTest : testing::Test {
  FakeStore store;
  FakeCache cache;
  ProductSharedState shared;
};

struct ProductBatchServiceTest : testing::Test {
  BatchStore store;
  BatchCache cache;
  ProductSharedState shared;
};

class RaceDatabase final {
 public:
  explicit RaceDatabase(Product row) : _row{std::move(row)} {}

  std::optional<Product> find(std::uint64_t id, bool pause_after_read) {
    std::optional<Product> snapshot;
    {
      std::lock_guard<std::mutex> lock{_row_mutex};
      ++_find_calls;
      if (_row.id == id) {
        snapshot = _row;
      }
    }

    if (snapshot && pause_after_read) {
      std::unique_lock<std::mutex> lock{_barrier_mutex};
      _old_read_paused = true;
      _barrier.notify_all();
      _barrier.wait(lock, [this] { return _release_old_read; });
    }
    return snapshot;
  }

  StoreUpdateResult update(const UpdateProductRequest& request) {
    std::lock_guard<std::mutex> lock{_row_mutex};
    if (_row.id != request.id) {
      return {StoreUpdateStatus::NotFound, std::nullopt};
    }
    if (_row.version != request.expected_version) {
      return {StoreUpdateStatus::Conflict, std::nullopt};
    }
    _row = {request.id, request.name, request.price_cents, request.expected_version + 1};
    return {StoreUpdateStatus::Updated, _row};
  }

  bool wait_until_old_read_paused(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock{_barrier_mutex};
    return _barrier.wait_for(lock, timeout, [this] { return _old_read_paused; });
  }

  void release_old_read() {
    {
      std::lock_guard<std::mutex> lock{_barrier_mutex};
      _release_old_read = true;
    }
    _barrier.notify_all();
  }

  int find_calls() const {
    std::lock_guard<std::mutex> lock{_row_mutex};
    return _find_calls;
  }

 private:
  mutable std::mutex _row_mutex;
  Product _row;
  int _find_calls = 0;
  std::mutex _barrier_mutex;
  std::condition_variable _barrier;
  bool _old_read_paused = false;
  bool _release_old_read = false;
};

class ManualCacheState final {
 public:
  std::optional<std::string> get() {
    std::lock_guard<std::mutex> lock{_mutex};
    if (!_value) {
      return std::nullopt;
    }
    if (_now >= _expires_at) {
      _value.reset();
      return std::nullopt;
    }
    return _value;
  }

  void put(std::string_view value, std::uint32_t ttl_seconds) {
    std::lock_guard<std::mutex> lock{_mutex};
    _value = std::string{value};
    _expires_at = _now + std::chrono::seconds{ttl_seconds};
  }

  void erase() {
    std::lock_guard<std::mutex> lock{_mutex};
    _value.reset();
  }

  void advance(std::chrono::seconds amount) {
    std::lock_guard<std::mutex> lock{_mutex};
    _now += amount;
  }

  std::chrono::seconds remaining_ttl() const {
    std::lock_guard<std::mutex> lock{_mutex};
    return _expires_at - _now;
  }

 private:
  mutable std::mutex _mutex;
  std::optional<std::string> _value;
  std::chrono::seconds _now{0};
  std::chrono::seconds _expires_at{0};
};

class RaceStore final : public ProductStore {
 public:
  RaceStore(std::shared_ptr<RaceDatabase> database, bool pause_after_read)
      : _database{std::move(database)}, _pause_after_read{pause_after_read} {}

  std::optional<Product> find(std::uint64_t id) override {
    return _database->find(id, _pause_after_read);
  }

  StoreUpdateResult update(const UpdateProductRequest& request) override {
    return _database->update(request);
  }

 private:
  std::shared_ptr<RaceDatabase> _database;
  bool _pause_after_read;
};

class RaceCache final : public ProductCache {
 public:
  explicit RaceCache(std::shared_ptr<ManualCacheState> state) : _state{std::move(state)} {}

  std::optional<std::string> get(std::string_view) override { return _state->get(); }

  void put(std::string_view, std::string_view value, std::uint32_t ttl_seconds) override {
    _state->put(value, ttl_seconds);
  }

  void erase(std::string_view) override { _state->erase(); }

 private:
  std::shared_ptr<ManualCacheState> _state;
};

TEST_F(ProductServiceTest, HitDoesNotReadDatabase) {
  cache.value = encode_product_cache({1, "tea", 199, 1});
  ProductService service{store, cache, shared};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, CacheSource::Hit);
  if (!result.product) {
    ADD_FAILURE() << "cache hit returned no product";
    return;
  }
  EXPECT_EQ(result.product->name, "tea");
  EXPECT_EQ(store.finds, 0);
}

TEST_F(ProductServiceTest, MissReadsDatabaseAndFillsWithRelativeTtl) {
  store.row = Product{1, "tea", 199, 1};
  ProductService service{store, cache, shared, {CachePolicyMode::Basic, 45, 5, 3}};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, CacheSource::Miss);
  EXPECT_EQ(store.finds, 1);
  EXPECT_EQ(cache.puts, 1);
  EXPECT_EQ(cache.last_ttl, 45U);
  if (!cache.value) {
    ADD_FAILURE() << "database result was not cached";
    return;
  }
  const auto decoded = decode_product_cache_entry(*cache.value, 1).product;
  if (!decoded) {
    ADD_FAILURE() << "cached product did not decode";
    return;
  }
  EXPECT_EQ(decoded->version, 1U);
}

TEST_F(ProductServiceTest, BasicPolicyRechecksNegativeCacheEntriesAgainstTheStore) {
  store.row = Product{1, "tea", 199, 1};
  cache.value = encode_product_not_found(1);
  ProductService service{store, cache, shared};

  const auto result = service.get(1);

  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, CacheSource::Miss);
  ASSERT_TRUE(result.product.has_value());
  EXPECT_EQ(result.product.value_or(Product{}).name, "tea");
  EXPECT_EQ(store.finds, 1);
  EXPECT_EQ(cache.erases, 0);
  ASSERT_TRUE(cache.value.has_value());
  const auto cached_product = decode_product_cache_entry(cache.value.value_or(""), 1).product;
  ASSERT_TRUE(cached_product.has_value());
  EXPECT_EQ(cached_product.value_or(Product{}).name, "tea");
}

TEST_F(ProductServiceTest, ProtectedPolicyServesNegativeCacheEntriesWithoutStoreReads) {
  cache.value = encode_product_not_found(9);
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;
  ProductService service{store, cache, shared, policy};

  const auto result = service.get(9);

  EXPECT_EQ(result.status, ProductStatus::NotFound);
  EXPECT_EQ(result.cache_source, CacheSource::Hit);
  EXPECT_FALSE(result.product.has_value());
  EXPECT_EQ(store.finds, 0);
  EXPECT_EQ(cache.erases, 0);
}

TEST_F(ProductServiceTest, ProtectedPolicyWritesNegativeEntriesWithTheirOwnTtl) {
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;
  policy.negative_ttl_seconds = 7;
  ProductService service{store, cache, shared, policy};

  const auto result = service.get(9);

  EXPECT_EQ(result.status, ProductStatus::NotFound);
  EXPECT_EQ(cache.puts, 1);
  EXPECT_EQ(cache.last_ttl, 7U);
  ASSERT_TRUE(cache.value.has_value());
  EXPECT_EQ(decode_product_cache_entry(cache.value.value_or(""), 9).kind, CacheEntryKind::NotFound);
}

TEST_F(ProductBatchServiceTest, ProtectedBatchWritesNegativeEntriesForMissingProducts) {
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;
  policy.negative_ttl_seconds = 7;
  ProductService service{store, cache, shared, policy};

  const auto result = service.get_many({9, 1, 9});

  ASSERT_EQ(result.items.size(), 3U);
  EXPECT_EQ(result.items[0].result.status, ProductStatus::NotFound);
  EXPECT_EQ(result.items[1].result.status, ProductStatus::NotFound);
  EXPECT_EQ(result.items[2].result.status, ProductStatus::NotFound);
  ASSERT_EQ(cache.batch_writes.size(), 2U);
  const std::vector<std::uint64_t> expected_ids{9, 1};
  for (std::size_t index = 0; index < cache.batch_writes.size(); ++index) {
    const auto& entry = cache.batch_writes[index];
    EXPECT_EQ(entry.key, make_product_cache_key(expected_ids[index]));
    EXPECT_EQ(entry.ttl_seconds, 7U);
    EXPECT_EQ(decode_product_cache_entry(entry.value, expected_ids[index]).kind,
              CacheEntryKind::NotFound);
  }
}

TEST_F(ProductServiceTest, ProtectedStoreFailureDoesNotWriteNegativeCache) {
  store.find_error = StoreErrorCode::Unavailable;
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;
  ProductService service{store, cache, shared, policy};

  EXPECT_EQ(service.get(9).status, ProductStatus::StoreUnavailable);
  EXPECT_EQ(cache.puts, 0);
}

TEST(ProductServiceConcurrencyTest, ProtectedFreshReadUsesAdmissionWithoutJoiningAFlight) {
  FakeStore store;
  store.row = Product{1, "tea", 199, 1};
  FakeCache cache;
  ProductReadOptions read_options;
  read_options.max_concurrent_loads = 1;
  ProductSharedState shared{read_options};
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;
  auto occupied_permit = shared.reads.try_acquire_load();
  ASSERT_TRUE(occupied_permit.has_value());
  ProductService service{store, cache, shared, policy};

  const auto busy = service.get(1, true);
  EXPECT_EQ(busy.status, ProductStatus::ReadBusy);
  EXPECT_EQ(busy.cache_source, CacheSource::Bypass);
  EXPECT_EQ(store.finds, 0);
  EXPECT_EQ(cache.gets, 0);
  EXPECT_EQ(shared.reads.active_key_count(), 0U);

  occupied_permit.reset();
  const auto fresh = service.get(1, true);
  EXPECT_EQ(fresh.status, ProductStatus::Ok);
  EXPECT_EQ(fresh.cache_source, CacheSource::Bypass);
  EXPECT_EQ(store.finds, 1);
  EXPECT_EQ(cache.puts, 1);
  EXPECT_EQ(shared.reads.active_key_count(), 0U);
  EXPECT_EQ(shared.metrics.snapshot()
                .counters[static_cast<std::size_t>(ProductMetric::ReadAdmissionRejected)],
            1U);
}

TEST_F(ProductServiceTest, ProtectedPolicyUsesStableJitterForPositiveCacheEntries) {
  store.row = Product{42, "tea", 199, 1};
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;
  policy.ttl_seconds = 30;
  policy.ttl_jitter_seconds = 3;
  ProductService service{store, cache, shared, policy};

  const auto result = service.get(42);

  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_GE(cache.last_ttl, 27U);
  EXPECT_LE(cache.last_ttl, 33U);
  EXPECT_EQ(cache.last_ttl, product_cache_ttl(42, policy));
}

TEST_F(ProductServiceTest, RejectsInvalidCachePolicyBounds) {
  ProductCachePolicy policy;
  policy.negative_ttl_seconds = 31;
  EXPECT_THROW((ProductService{store, cache, shared, policy}), std::invalid_argument);

  policy.negative_ttl_seconds = 5;
  policy.ttl_jitter_seconds = 31;
  EXPECT_THROW((ProductService{store, cache, shared, policy}), std::invalid_argument);
}

TEST_F(ProductServiceTest, CacheFailureBypassesAndDatabaseFailureIsUnavailable) {
  store.find_error = StoreErrorCode::Unavailable;
  cache.fail_get = true;
  ProductService service{store, cache, shared};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, ProductStatus::StoreUnavailable);
  EXPECT_EQ(result.cache_source, CacheSource::Bypass);
  EXPECT_EQ(store.finds, 1);
}

TEST_F(ProductServiceTest, CorruptOrWrongIdCacheValueIsErasedAndReplaced) {
  store.row = Product{1, "fresh", 300, 2};
  cache.value = encode_product_cache({2, "wrong", 100, 1});
  ProductService service{store, cache, shared};
  const auto result = service.get(1);
  EXPECT_EQ(result.cache_source, CacheSource::Corrupt);
  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_EQ(cache.erases, 1);
  if (!result.product) {
    ADD_FAILURE() << "database read returned no product";
    return;
  }
  EXPECT_EQ(result.product->version, 2U);
}

TEST_F(ProductServiceTest, MissingRowIsNotNegativelyCached) {
  ProductService service{store, cache, shared};
  EXPECT_EQ(service.get(1).status, ProductStatus::NotFound);
  EXPECT_EQ(cache.puts, 0);
}

TEST_F(ProductServiceTest, FreshReadBypassesStaleCacheForCommitReconciliation) {
  store.row = Product{1, "new", 300, 2};
  cache.value = encode_product_cache({1, "old", 200, 1});
  ProductService service{store, cache, shared};
  const auto result = service.get(1, true);
  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, CacheSource::Bypass);
  if (!result.product) {
    ADD_FAILURE() << "fresh read returned no product";
    return;
  }
  EXPECT_EQ(result.product->version, 2U);
  EXPECT_EQ(cache.gets, 0);
  EXPECT_EQ(store.finds, 1);
}

TEST_F(ProductServiceTest, SuccessfulUpdateCommitsBeforeInvalidation) {
  store.next_update = {StoreUpdateStatus::Updated, Product{1, "new", 250, 2}};
  cache.value = encode_product_cache({1, "old", 200, 1});
  ProductService service{store, cache, shared};
  const auto result = service.update({1, "new", 250, 1});
  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_FALSE(result.cache_invalidation_failed);
  EXPECT_EQ(store.updates, 1);
  EXPECT_EQ(cache.erases, 1);
  EXPECT_FALSE(cache.value);
}

TEST_F(ProductServiceTest, CommittedUpdateSurvivesCacheFailure) {
  store.next_update = {StoreUpdateStatus::Updated, Product{1, "new", 250, 2}};
  cache.fail_erase = true;
  ProductService service{store, cache, shared};
  const auto result = service.update({1, "new", 250, 1});
  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_TRUE(result.cache_invalidation_failed);
}

TEST_F(ProductServiceTest, ConflictAndUnknownCommitDoNotTouchCache) {
  ProductService service{store, cache, shared};
  store.next_update = {StoreUpdateStatus::Conflict, std::nullopt};
  EXPECT_EQ(service.update({1, "new", 250, 1}).status, ProductStatus::Conflict);
  store.update_error = StoreErrorCode::CommitUnknown;
  EXPECT_EQ(service.update({1, "new", 250, 1}).status, ProductStatus::CommitUnknown);
  EXPECT_EQ(cache.erases, 0);
  EXPECT_EQ(store.updates, 2);
}

TEST_F(ProductServiceTest, InvalidInputDoesNotUseDependencies) {
  ProductService service{store, cache, shared};
  EXPECT_EQ(service.get(0).status, ProductStatus::InvalidArgument);
  EXPECT_EQ(service.update({1, "", 1, 1}).status, ProductStatus::InvalidArgument);
  EXPECT_EQ(store.finds, 0);
  EXPECT_EQ(store.updates, 0);
  EXPECT_EQ(cache.gets, 0);
}

TEST(ProductServiceConcurrencyTest, ConcurrentStaleFillExpiresByOwnTtl) {
  constexpr std::uint64_t product_id = 42;
  constexpr std::uint32_t ttl_seconds = 5;
  auto database = std::make_shared<RaceDatabase>(Product{product_id, "old", 100, 1});
  auto cache_state = std::make_shared<ManualCacheState>();

  RaceStore old_read_store{database, true};
  RaceCache old_read_cache{cache_state};
  ProductSharedState shared;
  ProductService old_reader{
      old_read_store, old_read_cache, shared, {CachePolicyMode::Basic, ttl_seconds, 5, 3}};

  RaceStore update_store{database, false};
  RaceCache update_cache{cache_state};
  ProductService updater{
      update_store, update_cache, shared, {CachePolicyMode::Basic, ttl_seconds, 5, 3}};

  std::optional<GetProductResult> old_read_result;
  std::exception_ptr old_read_error;
  std::thread old_read_thread{[&] {
    try {
      old_read_result = old_reader.get(product_id);
    } catch (...) {
      old_read_error = std::current_exception();
    }
  }};

  const bool old_read_paused = database->wait_until_old_read_paused(std::chrono::seconds{2});
  if (!old_read_paused) {
    database->release_old_read();
    old_read_thread.join();
    FAIL() << "old GET did not stop after capturing the version 1 database row";
    return;
  }

  const auto update_result = updater.update({product_id, "new", 200, 1});
  EXPECT_EQ(update_result.status, ProductStatus::Ok);
  EXPECT_TRUE(update_result.product);
  if (update_result.product) {
    EXPECT_EQ(update_result.product.value().version, 2U);
  }
  EXPECT_FALSE(cache_state->get());

  // 模拟旧读请求拿到数据库快照后长时间暂停：写请求已提交并删缓存，旧读才恢复并回填。
  // 旧值的 TTL 从这次回填开始计算，而不是从先前的数据库读取时间开始计算。
  cache_state->advance(std::chrono::seconds{100});
  database->release_old_read();
  old_read_thread.join();
  if (old_read_error) {
    std::rethrow_exception(old_read_error);
  }
  if (!old_read_result) {
    ADD_FAILURE() << "old read returned no result";
    return;
  }
  ASSERT_EQ(old_read_result->status, ProductStatus::Ok);
  if (!old_read_result->product) {
    ADD_FAILURE() << "old read returned no product";
    return;
  }
  EXPECT_EQ(old_read_result->product->version, 1U);
  EXPECT_EQ(cache_state->remaining_ttl(), std::chrono::seconds{ttl_seconds});

  RaceStore later_read_store{database, false};
  RaceCache later_read_cache{cache_state};
  ProductService later_reader{
      later_read_store, later_read_cache, shared, {CachePolicyMode::Basic, ttl_seconds, 5, 3}};
  auto stale_hit = later_reader.get(product_id);
  ASSERT_EQ(stale_hit.status, ProductStatus::Ok);
  if (!stale_hit.product) {
    ADD_FAILURE() << "stale cache hit returned no product";
    return;
  }
  EXPECT_EQ(stale_hit.cache_source, CacheSource::Hit);
  EXPECT_EQ(stale_hit.product->version, 1U);
  EXPECT_EQ(database->find_calls(), 1);

  cache_state->advance(std::chrono::seconds{ttl_seconds - 1});
  stale_hit = later_reader.get(product_id);
  ASSERT_EQ(stale_hit.status, ProductStatus::Ok);
  if (!stale_hit.product) {
    ADD_FAILURE() << "stale cache hit returned no product";
    return;
  }
  EXPECT_EQ(stale_hit.cache_source, CacheSource::Hit);
  EXPECT_EQ(stale_hit.product->version, 1U);
  EXPECT_EQ(cache_state->remaining_ttl(), std::chrono::seconds{1});

  cache_state->advance(std::chrono::seconds{1});
  const auto refreshed = later_reader.get(product_id);
  ASSERT_EQ(refreshed.status, ProductStatus::Ok);
  if (!refreshed.product) {
    ADD_FAILURE() << "refreshed read returned no product";
    return;
  }
  EXPECT_EQ(refreshed.cache_source, CacheSource::Miss);
  EXPECT_EQ(refreshed.product->version, 2U);
  EXPECT_EQ(refreshed.product->name, "new");
  EXPECT_EQ(database->find_calls(), 2);

  const auto fresh_hit = later_reader.get(product_id);
  ASSERT_EQ(fresh_hit.status, ProductStatus::Ok);
  if (!fresh_hit.product) {
    ADD_FAILURE() << "fresh cache hit returned no product";
    return;
  }
  EXPECT_EQ(fresh_hit.cache_source, CacheSource::Hit);
  EXPECT_EQ(fresh_hit.product->version, 2U);
}

TEST(ProductServiceConcurrencyTest, ProtectedFollowerTimeoutDoesNotCancelItsLeader) {
  constexpr std::uint64_t product_id = 42;
  auto database = std::make_shared<RaceDatabase>(Product{product_id, "tea", 199, 1});
  auto cache_state = std::make_shared<ManualCacheState>();
  ProductReadOptions read_options;
  read_options.wait_timeout = std::chrono::milliseconds{20};
  ProductSharedState shared{read_options};
  ProductCachePolicy policy;
  policy.mode = CachePolicyMode::Protected;

  RaceStore leader_store{database, true};
  RaceCache leader_cache{cache_state};
  ProductService leader{leader_store, leader_cache, shared, policy};
  RaceStore follower_store{database, false};
  RaceCache follower_cache{cache_state};
  ProductService follower{follower_store, follower_cache, shared, policy};

  std::optional<GetProductResult> leader_result;
  std::exception_ptr leader_error;
  std::thread leader_thread{[&] {
    try {
      leader_result = leader.get(product_id);
    } catch (...) {
      leader_error = std::current_exception();
    }
  }};

  if (!database->wait_until_old_read_paused(std::chrono::seconds{2})) {
    database->release_old_read();
    leader_thread.join();
    FAIL() << "leader did not pause after entering the store";
    return;
  }

  const auto follower_result = follower.get(product_id);
  EXPECT_EQ(follower_result.status, ProductStatus::ReadBusy);
  EXPECT_EQ(follower_result.cache_source, CacheSource::Miss);
  EXPECT_EQ(database->find_calls(), 1);
  EXPECT_EQ(shared.reads.active_key_count(), 1U);
  EXPECT_EQ(shared.reads.active_load_count(), 1U);

  database->release_old_read();
  leader_thread.join();
  if (leader_error) {
    std::rethrow_exception(leader_error);
  }
  ASSERT_TRUE(leader_result.has_value());
  EXPECT_EQ(leader_result.value_or(GetProductResult{}).status, ProductStatus::Ok);
  EXPECT_EQ(shared.reads.active_key_count(), 0U);
  EXPECT_EQ(shared.reads.active_load_count(), 0U);
  const auto metrics = shared.metrics.snapshot();
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::ReadLeaders)], 1U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::ReadFollowers)], 1U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::ReadWaitTimeouts)], 1U);
}

TEST_F(ProductBatchServiceTest, BatchMissUsesOneStoreCallAndRestoresDuplicateItems) {
  store.rows.emplace(1, Product{1, "tea", 199, 1});
  store.rows.emplace(2, Product{2, "coffee", 299, 1});
  ProductService service{store, cache, shared};

  const auto result = service.get_many({2, 99, 1, 2});

  EXPECT_EQ(result.status, ProductStatus::Ok);
  ASSERT_EQ(result.items.size(), 4U);
  EXPECT_EQ(result.items[0].id, 2U);
  EXPECT_EQ(result.items[0].result.status, ProductStatus::Ok);
  EXPECT_EQ(result.items[0].result.product.value_or(Product{}).id, 2U);
  EXPECT_EQ(result.items[1].id, 99U);
  EXPECT_EQ(result.items[1].result.status, ProductStatus::NotFound);
  EXPECT_EQ(result.items[2].id, 1U);
  EXPECT_EQ(result.items[2].result.product.value_or(Product{}).id, 1U);
  EXPECT_EQ(result.items[3].id, 2U);
  EXPECT_EQ(result.items[3].result.product.value_or(Product{}).id, 2U);
  EXPECT_EQ(store.batch_queries, (std::vector<std::vector<std::uint64_t>>{{2, 99, 1}}));
  EXPECT_EQ(store.finds, 0);
  EXPECT_EQ(
      cache.batch_reads,
      (std::vector<std::vector<std::string>>{{"product:v3:2", "product:v3:99", "product:v3:1"}}));
  ASSERT_EQ(cache.batch_writes.size(), 2U);
  EXPECT_EQ(cache.batch_writes[0].key, "product:v3:2");
  EXPECT_EQ(cache.batch_writes[1].key, "product:v3:1");
}

TEST_F(ProductBatchServiceTest, BatchCacheHitsLeaveOnlyMissesForTheStore) {
  store.rows.emplace(2, Product{2, "coffee", 299, 1});
  cache.values.emplace("product:v3:1", encode_product_cache({1, "tea", 199, 1}));
  ProductService service{store, cache, shared};

  const auto result = service.get_many({1, 2, 1});

  EXPECT_EQ(result.status, ProductStatus::Ok);
  ASSERT_EQ(result.items.size(), 3U);
  EXPECT_EQ(result.items[0].result.cache_source, CacheSource::Hit);
  EXPECT_EQ(result.items[1].result.cache_source, CacheSource::Miss);
  EXPECT_EQ(result.items[2].result.product.value_or(Product{}).id, 1U);
  EXPECT_EQ(store.batch_queries, (std::vector<std::vector<std::uint64_t>>{{2}}));
  EXPECT_EQ(store.finds, 0);
  ASSERT_EQ(cache.batch_writes.size(), 1U);
  EXPECT_EQ(cache.batch_writes[0].key, "product:v3:2");
}

TEST_F(ProductBatchServiceTest, MetricsCountUniqueCacheAndStoreOperations) {
  store.rows.emplace(1, Product{1, "coffee", 299, 1});
  cache.values.emplace("product:v3:2", encode_product_cache({2, "tea", 199, 1}));
  ProductService service{store, cache, shared};

  const auto result = service.get_many({2, 1, 2});

  ASSERT_EQ(result.items.size(), 3U);
  const auto metrics = shared.metrics.snapshot();
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::BatchRequests)], 1U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::RequestUniqueIds)], 2U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::CacheLookupKeys)], 2U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::CacheHits)], 1U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::CacheMisses)], 1U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::StoreReadOperations)], 1U);
  EXPECT_EQ(metrics.counters[static_cast<std::size_t>(ProductMetric::StoreReadIds)], 1U);
}

TEST_F(ProductBatchServiceTest, CacheBatchFailureFallsBackForEveryUnresolvedItem) {
  store.rows.emplace(1, Product{1, "tea", 199, 1});
  store.rows.emplace(2, Product{2, "coffee", 299, 1});
  cache.fail_batch_read = true;
  ProductService service{store, cache, shared};

  const auto result = service.get_many({1, 2});

  ASSERT_EQ(result.items.size(), 2U);
  EXPECT_EQ(result.items[0].result.status, ProductStatus::Ok);
  EXPECT_EQ(result.items[0].result.cache_source, CacheSource::Bypass);
  EXPECT_EQ(result.items[1].result.cache_source, CacheSource::Bypass);
  EXPECT_EQ(store.batch_queries, (std::vector<std::vector<std::uint64_t>>{{1, 2}}));
}

TEST_F(ProductBatchServiceTest, StoreBatchFailureDoesNotReplaceEarlierCacheHits) {
  store.batch_error = StoreErrorCode::Unavailable;
  cache.values.emplace("product:v3:1", encode_product_cache({1, "tea", 199, 1}));
  ProductService service{store, cache, shared};

  const auto result = service.get_many({1, 2});

  ASSERT_EQ(result.items.size(), 2U);
  EXPECT_EQ(result.items[0].result.status, ProductStatus::Ok);
  EXPECT_EQ(result.items[0].result.cache_source, CacheSource::Hit);
  EXPECT_EQ(result.items[1].result.status, ProductStatus::StoreUnavailable);
  EXPECT_EQ(result.items[1].result.cache_source, CacheSource::Miss);
  EXPECT_TRUE(cache.batch_writes.empty());
}

TEST_F(ProductBatchServiceTest, InvalidBatchSkipsDependenciesAndFreshBatchRefillsCache) {
  store.rows.emplace(1, Product{1, "tea", 199, 1});
  ProductService service{store, cache, shared};

  const auto invalid = service.get_many({1, 0});
  EXPECT_EQ(invalid.status, ProductStatus::InvalidArgument);
  EXPECT_TRUE(invalid.items.empty());
  const std::vector<std::uint64_t> oversized(max_product_batch_size + 1, 1);
  EXPECT_EQ(service.get_many(oversized).status, ProductStatus::InvalidArgument);
  const auto empty = service.get_many({});
  EXPECT_EQ(empty.status, ProductStatus::Ok);
  EXPECT_TRUE(empty.items.empty());

  const auto fresh = service.get_many({1, 1}, true);
  ASSERT_EQ(fresh.items.size(), 2U);
  EXPECT_EQ(fresh.items[0].result.cache_source, CacheSource::Bypass);
  EXPECT_EQ(fresh.items[1].result.product.value_or(Product{}).id, 1U);
  EXPECT_TRUE(cache.batch_reads.empty());
  EXPECT_TRUE(cache.batch_writes.empty());
  EXPECT_NE(cache.values.find("product:v3:1"), cache.values.end());
  EXPECT_TRUE(store.batch_queries.empty());
  EXPECT_EQ(store.finds, 1);
}

}  // namespace
}  // namespace sphinx
