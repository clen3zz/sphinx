// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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

class RaceDatabase final {
 public:
  explicit RaceDatabase(sphinx::Product row) : _row{std::move(row)} {}

  std::optional<sphinx::Product> find(std::uint64_t id, bool pause_after_read) {
    std::optional<sphinx::Product> snapshot;
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

  sphinx::StoreUpdateResult update(const sphinx::UpdateProductRequest& request) {
    std::lock_guard<std::mutex> lock{_row_mutex};
    if (_row.id != request.id) {
      return {sphinx::StoreUpdateStatus::NotFound, std::nullopt};
    }
    if (_row.version != request.expected_version) {
      return {sphinx::StoreUpdateStatus::Conflict, std::nullopt};
    }
    _row = {request.id, request.name, request.price_cents, request.expected_version + 1};
    return {sphinx::StoreUpdateStatus::Updated, _row};
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
  sphinx::Product _row;
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

class RaceStore final : public sphinx::ProductStore {
 public:
  RaceStore(std::shared_ptr<RaceDatabase> database, bool pause_after_read)
      : _database{std::move(database)}, _pause_after_read{pause_after_read} {}

  std::optional<sphinx::Product> find(std::uint64_t id) override {
    return _database->find(id, _pause_after_read);
  }

  sphinx::StoreUpdateResult update(const sphinx::UpdateProductRequest& request) override {
    return _database->update(request);
  }

 private:
  std::shared_ptr<RaceDatabase> _database;
  bool _pause_after_read;
};

class RaceCache final : public sphinx::ProductCache {
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

TEST(ProductServiceTest, HitDoesNotReadDatabase) {
  FakeStore store;
  FakeCache cache;
  cache.value = sphinx::encode_product_cache({1, "tea", 199, 1});
  sphinx::ProductService service{store, cache};
  const auto result = service.get(1);
  EXPECT_EQ(result.status, sphinx::ProductStatus::Ok);
  EXPECT_EQ(result.cache_source, sphinx::CacheSource::Hit);
  if (!result.product) {
    ADD_FAILURE() << "cache hit returned no product";
    return;
  }
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
  if (!cache.value) {
    ADD_FAILURE() << "database result was not cached";
    return;
  }
  const auto decoded = sphinx::decode_product_cache(*cache.value);
  if (!decoded) {
    ADD_FAILURE() << "cached product did not decode";
    return;
  }
  EXPECT_EQ(decoded->version, 1U);
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
  if (!result.product) {
    ADD_FAILURE() << "database read returned no product";
    return;
  }
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
  if (!result.product) {
    ADD_FAILURE() << "fresh read returned no product";
    return;
  }
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
  constexpr std::uint64_t product_id = 42;
  constexpr std::uint32_t ttl_seconds = 5;
  auto database = std::make_shared<RaceDatabase>(sphinx::Product{product_id, "old", 100, 1});
  auto cache_state = std::make_shared<ManualCacheState>();

  RaceStore old_read_store{database, true};
  RaceCache old_read_cache{cache_state};
  sphinx::ProductService old_reader{old_read_store, old_read_cache, {ttl_seconds}};

  RaceStore update_store{database, false};
  RaceCache update_cache{cache_state};
  sphinx::ProductService updater{update_store, update_cache, {ttl_seconds}};

  std::optional<sphinx::GetProductResult> old_read_result;
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
  EXPECT_EQ(update_result.status, sphinx::ProductStatus::Ok);
  EXPECT_TRUE(update_result.product);
  if (update_result.product) {
    EXPECT_EQ(update_result.product.value().version, 2U);
  }
  EXPECT_FALSE(cache_state->get());

  // Model a long pause after the old database snapshot. The stale cache entry's TTL starts only
  // when that GET resumes and writes it, after the committed update has already erased the key.
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
  ASSERT_EQ(old_read_result->status, sphinx::ProductStatus::Ok);
  if (!old_read_result->product) {
    ADD_FAILURE() << "old read returned no product";
    return;
  }
  EXPECT_EQ(old_read_result->product->version, 1U);
  EXPECT_EQ(cache_state->remaining_ttl(), std::chrono::seconds{ttl_seconds});

  RaceStore later_read_store{database, false};
  RaceCache later_read_cache{cache_state};
  sphinx::ProductService later_reader{later_read_store, later_read_cache, {ttl_seconds}};
  auto stale_hit = later_reader.get(product_id);
  ASSERT_EQ(stale_hit.status, sphinx::ProductStatus::Ok);
  if (!stale_hit.product) {
    ADD_FAILURE() << "stale cache hit returned no product";
    return;
  }
  EXPECT_EQ(stale_hit.cache_source, sphinx::CacheSource::Hit);
  EXPECT_EQ(stale_hit.product->version, 1U);
  EXPECT_EQ(database->find_calls(), 1);

  cache_state->advance(std::chrono::seconds{ttl_seconds - 1});
  stale_hit = later_reader.get(product_id);
  ASSERT_EQ(stale_hit.status, sphinx::ProductStatus::Ok);
  if (!stale_hit.product) {
    ADD_FAILURE() << "stale cache hit returned no product";
    return;
  }
  EXPECT_EQ(stale_hit.cache_source, sphinx::CacheSource::Hit);
  EXPECT_EQ(stale_hit.product->version, 1U);
  EXPECT_EQ(cache_state->remaining_ttl(), std::chrono::seconds{1});

  cache_state->advance(std::chrono::seconds{1});
  const auto refreshed = later_reader.get(product_id);
  ASSERT_EQ(refreshed.status, sphinx::ProductStatus::Ok);
  if (!refreshed.product) {
    ADD_FAILURE() << "refreshed read returned no product";
    return;
  }
  EXPECT_EQ(refreshed.cache_source, sphinx::CacheSource::Miss);
  EXPECT_EQ(refreshed.product->version, 2U);
  EXPECT_EQ(refreshed.product->name, "new");
  EXPECT_EQ(database->find_calls(), 2);

  const auto fresh_hit = later_reader.get(product_id);
  ASSERT_EQ(fresh_hit.status, sphinx::ProductStatus::Ok);
  if (!fresh_hit.product) {
    ADD_FAILURE() << "fresh cache hit returned no product";
    return;
  }
  EXPECT_EQ(fresh_hit.cache_source, sphinx::CacheSource::Hit);
  EXPECT_EQ(fresh_hit.product->version, 2U);
}

}  // namespace
