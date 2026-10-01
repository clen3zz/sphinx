// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace sphinx {
namespace {

constexpr std::uint32_t max_negative_cache_ttl_seconds = 30;
constexpr std::uint32_t max_product_cache_ttl_jitter_seconds = 30;

ProductStatus status_from_store_error(StoreErrorCode code) noexcept {
  switch (code) {
    case StoreErrorCode::Unavailable:
      return ProductStatus::StoreUnavailable;
    case StoreErrorCode::CommitUnknown:
      return ProductStatus::CommitUnknown;
    case StoreErrorCode::InvalidData:
    case StoreErrorCode::Unexpected:
      return ProductStatus::InternalError;
  }
  return ProductStatus::InternalError;
}

// 各种缓存操作共用熔断许可和失败计数；业务层仍决定失败后的回源或回填策略。
template <typename Operation>
bool run_cache_operation(ProductSharedState& shared, CachePolicyMode mode,
                         ProductMetric failure_metric, Operation operation) {
  std::optional<CacheOperationPermit> permit;
  if (mode == CachePolicyMode::Protected) {
    permit = shared.breaker.try_acquire();
    if (!permit) {
      shared.metrics.increment(ProductMetric::CacheCircuitBypasses);
      return false;
    }
  }
  try {
    operation();
    if (permit) {
      permit->succeed();
    }
    return true;
  } catch (const CacheError&) {
    if (permit) {
      permit->fail();
    }
    shared.metrics.increment(failure_metric);
    return false;
  }
}

}  // namespace

void validate_product_cache_policy(const ProductCachePolicy& policy) {
  const bool valid_mode =
      policy.mode == CachePolicyMode::Basic || policy.mode == CachePolicyMode::Protected;
  if (!valid_mode || !valid_product_cache_ttl(policy.ttl_seconds) ||
      policy.negative_ttl_seconds < 1 ||
      policy.negative_ttl_seconds > max_negative_cache_ttl_seconds ||
      policy.ttl_jitter_seconds > max_product_cache_ttl_jitter_seconds) {
    throw std::invalid_argument{"invalid product cache policy"};
  }
}

struct ProductService::ReadWorkItem {
  std::uint64_t id = 0;
  std::string key;
  CacheSource source = CacheSource::NotChecked;
  std::optional<GetProductResult> result;
  ProductReadTicket ticket;
};

struct ProductService::ReadBatch {
  std::vector<ReadWorkItem> work_items;
  std::vector<std::size_t> input_positions;
  bool bypass_cache = false;
  bool cache_failed = false;
};

ProductService::ProductService(ProductStore& store, ProductCache& cache, ProductSharedState& shared,
                               ProductCachePolicy policy)
    : _store{store}, _cache{cache}, _shared{shared}, _policy{policy} {
  validate_product_cache_policy(_policy);
}

GetProductResult ProductService::get(std::uint64_t id, bool bypass_cache) {
  _shared.metrics.increment(ProductMetric::GetRequests);

  if (id == 0) {
    return {ProductStatus::InvalidArgument, std::nullopt, CacheSource::NotChecked};
  }
  auto results = read_products({id}, bypass_cache);
  return std::move(results.front());
}

GetProductsResult ProductService::get_many(const std::vector<std::uint64_t>& ids,
                                           bool bypass_cache) {
  _shared.metrics.increment(ProductMetric::BatchRequests);

  if (ids.size() > max_product_batch_size) {
    return {ProductStatus::InvalidArgument, {}};
  }
  for (const auto id : ids) {
    if (id == 0) {
      return {ProductStatus::InvalidArgument, {}};
    }
  }
  auto results = read_products(ids, bypass_cache);
  GetProductsResult batch;
  batch.status = ProductStatus::Ok;
  batch.items.reserve(ids.size());
  for (std::size_t index = 0; index < ids.size(); ++index) {
    batch.items.push_back({ids[index], std::move(results[index])});
  }
  return batch;
}

std::vector<GetProductResult> ProductService::read_products(const std::vector<std::uint64_t>& ids,
                                                            bool bypass_cache) {
  ReadBatch batch = make_read_batch(ids, bypass_cache);
  _shared.metrics.increment(ProductMetric::RequestUniqueIds, batch.work_items.size());
  std::vector<std::size_t> positions(batch.work_items.size());
  std::iota(positions.begin(), positions.end(), std::size_t{0});

  if (!bypass_cache) {
    read_cache(batch, positions);
  }
  std::vector<std::size_t> unresolved;
  unresolved.reserve(batch.work_items.size());
  for (std::size_t index = 0; index < batch.work_items.size(); ++index) {
    if (!batch.work_items[index].result) {
      unresolved.push_back(index);
    }
  }
  if (_policy.mode == CachePolicyMode::Protected) {
    load_protected(batch, unresolved);
  } else {
    load_batch(batch, unresolved);
  }
  return restore_results(batch);
}

ProductService::ReadBatch ProductService::make_read_batch(const std::vector<std::uint64_t>& ids,
                                                          bool bypass_cache) const {
  ReadBatch batch;
  batch.bypass_cache = bypass_cache;
  batch.input_positions.reserve(ids.size());
  batch.work_items.reserve(ids.size());
  std::unordered_map<std::uint64_t, std::size_t> unique_positions;
  unique_positions.reserve(ids.size());

  for (const auto id : ids) {
    const auto [position, inserted] = unique_positions.emplace(id, batch.work_items.size());
    if (inserted) {
      batch.work_items.push_back({id,
                                  make_product_cache_key(id),
                                  bypass_cache ? CacheSource::Bypass : CacheSource::Miss,
                                  std::nullopt,
                                  {}});
    }
    batch.input_positions.push_back(position->second);
  }
  return batch;
}

void ProductService::read_cache(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  if (positions.empty()) {
    return;
  }

  std::vector<std::string> keys;
  keys.reserve(positions.size());
  for (const auto position : positions) {
    keys.push_back(batch.work_items[position].key);
  }

  std::vector<std::optional<std::string>> values;
  const bool succeeded =
      run_cache_operation(_shared, _policy.mode, ProductMetric::CacheReadFailures, [&] {
        _shared.metrics.increment(ProductMetric::CacheLookupKeys, positions.size());
        if (batch.work_items.size() == 1) {
          values.push_back(_cache.get(keys.front()));
        } else {
          values = _cache.get_many(keys);
        }
        if (values.size() != positions.size()) {
          throw CacheError{"cache batch returned an unexpected result count"};
        }
      });
  if (!succeeded) {
    batch.cache_failed = true;
    for (const auto position : positions) {
      batch.work_items[position].source = CacheSource::Bypass;
    }
    return;
  }

  for (std::size_t index = 0; index < positions.size(); ++index) {
    auto& item = batch.work_items[positions[index]];
    const auto& value = values[index];
    if (!value) {
      _shared.metrics.increment(ProductMetric::CacheMisses);
      item.source = CacheSource::Miss;
      continue;
    }
    auto entry = decode_product_cache_entry(*value, item.id);
    if (entry.kind == CacheEntryKind::Product && entry.product) {
      _shared.metrics.increment(ProductMetric::CacheHits);
      item.source = CacheSource::Hit;
      item.result = GetProductResult{ProductStatus::Ok, std::move(entry.product), item.source};
      continue;
    }
    if (entry.kind == CacheEntryKind::NotFound) {
      if (_policy.mode == CachePolicyMode::Protected) {
        _shared.metrics.increment(ProductMetric::NegativeHits);
        item.source = CacheSource::Hit;
        item.result = GetProductResult{ProductStatus::NotFound, std::nullopt, item.source};
      } else {
        _shared.metrics.increment(ProductMetric::CacheMisses);
        item.source = CacheSource::Miss;
      }
      continue;
    }
    _shared.metrics.increment(ProductMetric::CacheCorrupt);
    erase_corrupt(batch, positions[index]);
  }
}

void ProductService::erase_corrupt(ReadBatch& batch, std::size_t position) {
  auto& item = batch.work_items[position];
  item.source = CacheSource::Corrupt;
  if (_policy.mode == CachePolicyMode::Protected && batch.cache_failed) {
    return;
  }
  const bool succeeded = run_cache_operation(
      _shared, _policy.mode, ProductMetric::CacheCleanupFailures, [&] { _cache.erase(item.key); });
  if (!succeeded && _policy.mode == CachePolicyMode::Protected) {
    batch.cache_failed = true;
  }
}

void ProductService::load_batch(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  if (positions.empty()) {
    return;
  }
  {
    std::optional<ProductLoadPermit> permit;
    if (_policy.mode == CachePolicyMode::Protected) {
      permit = _shared.reads.try_acquire_load();
    }
    if (_policy.mode == CachePolicyMode::Protected && !permit) {
      _shared.metrics.increment(ProductMetric::ReadAdmissionRejected);
      for (const auto position : positions) {
        auto& item = batch.work_items[position];
        item.result = GetProductResult{ProductStatus::ReadBusy, std::nullopt, item.source};
      }
      return;
    }

    load_from_store(batch, positions);
  }
  // 数据库名额只覆盖回源；缓存 I/O 不占用名额。
  fill_cache(batch, positions);
}

void ProductService::load_protected(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  if (positions.empty()) {
    return;
  }
  if (batch.bypass_cache) {
    load_batch(batch, positions);
    return;
  }

  std::vector<std::uint64_t> ids;
  ids.reserve(positions.size());
  for (const auto position : positions) {
    ids.push_back(batch.work_items[position].id);
  }
  auto tickets = _shared.reads.acquire_many(ids);
  std::vector<std::size_t> leaders;
  std::vector<std::size_t> followers;
  leaders.reserve(positions.size());
  followers.reserve(positions.size());

  for (std::size_t index = 0; index < tickets.size(); ++index) {
    const auto position = positions[index];
    auto& item = batch.work_items[position];
    item.ticket = std::move(tickets[index]);
    switch (item.ticket.role()) {
      case ReadRole::Leader:
        _shared.metrics.increment(ProductMetric::ReadLeaders);
        leaders.push_back(position);
        break;
      case ReadRole::Follower:
        _shared.metrics.increment(ProductMetric::ReadFollowers);
        followers.push_back(position);
        break;
      case ReadRole::Rejected: {
        _shared.metrics.increment(ProductMetric::ReadRejected);
        item.result = GetProductResult{ProductStatus::ReadBusy, std::nullopt, item.source};
        break;
      }
    }
  }

  if (!batch.cache_failed) {
    read_cache(batch, leaders);
  }

  std::vector<std::size_t> load_positions;
  load_positions.reserve(leaders.size());
  for (const auto position : leaders) {
    if (!batch.work_items[position].result) {
      load_positions.push_back(position);
    }
  }
  load_batch(batch, load_positions);

  for (const auto position : leaders) {
    auto& item = batch.work_items[position];
    const auto result = item.result.value_or(
        GetProductResult{ProductStatus::InternalError, std::nullopt, item.source});
    item.ticket.complete({result.status, result.product});
  }

  const auto deadline = std::chrono::steady_clock::now() + _shared.reads.wait_timeout();
  if (!followers.empty()) {
    for (const auto position : followers) {
      auto& item = batch.work_items[position];
      const auto result = item.ticket.wait_until(deadline);
      if (item.ticket.wait_timed_out()) {
        _shared.metrics.increment(ProductMetric::ReadWaitTimeouts);
      }
      item.result = GetProductResult{result.status, result.product, item.source};
    }
  }
}

void ProductService::load_from_store(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  std::vector<std::uint64_t> ids;
  ids.reserve(positions.size());
  for (const auto position : positions) {
    ids.push_back(batch.work_items[position].id);
  }

  std::vector<std::optional<Product>> products;
  std::optional<ProductStatus> failure;
  _shared.metrics.increment(ProductMetric::StoreReadOperations);
  _shared.metrics.increment(ProductMetric::StoreReadIds, static_cast<std::uint64_t>(ids.size()));
  try {
    if (batch.work_items.size() == 1) {
      products.push_back(_store.find(ids.front()));
    } else {
      products = _store.find_many(ids);
    }
  } catch (const StoreError& error) {
    failure = status_from_store_error(error.code());
  } catch (...) {
    _shared.metrics.increment(ProductMetric::StoreReadFailures);
    throw;
  }

  if (failure || products.size() != ids.size()) {
    _shared.metrics.increment(ProductMetric::StoreReadFailures);
    for (const auto position : positions) {
      auto& item = batch.work_items[position];
      item.result = GetProductResult{failure.value_or(ProductStatus::InternalError), std::nullopt,
                                     item.source};
    }
    return;
  }

  bool invalid_row = false;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    auto& item = batch.work_items[positions[index]];
    auto& product = products[index];
    if (!product) {
      item.result = GetProductResult{ProductStatus::NotFound, std::nullopt, item.source};
    } else if (!valid_product(*product) || product->id != item.id) {
      invalid_row = true;
      item.result = GetProductResult{ProductStatus::InternalError, std::nullopt, item.source};
    } else {
      item.result = GetProductResult{ProductStatus::Ok, std::move(product), item.source};
    }
  }
  if (invalid_row) {
    _shared.metrics.increment(ProductMetric::StoreReadFailures);
  }
}

void ProductService::fill_cache(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  if (_policy.mode == CachePolicyMode::Protected && batch.cache_failed) {
    return;
  }

  std::vector<CacheWriteEntry> entries;
  entries.reserve(positions.size());
  for (const auto position : positions) {
    const auto& item = batch.work_items[position];
    if (item.result && item.result->status == ProductStatus::Ok && item.result->product) {
      entries.push_back({item.key, encode_product_cache(*item.result->product),
                         product_cache_ttl(item.id, _policy)});
    } else if (_policy.mode == CachePolicyMode::Protected && item.result &&
               item.result->status == ProductStatus::NotFound) {
      entries.push_back(
          {item.key, encode_product_not_found(item.id), _policy.negative_ttl_seconds});
    }
  }
  if (entries.empty()) {
    return;
  }

  // 回填失败只影响后续查询，不改变本次商品结果。
  (void)run_cache_operation(_shared, _policy.mode, ProductMetric::CacheFillFailures, [&] {
    if (batch.work_items.size() == 1) {
      const auto& entry = entries.front();
      _cache.put(entry.key, entry.value, entry.ttl_seconds);
    } else {
      _cache.put_many(entries);
    }
  });
}

std::vector<GetProductResult> ProductService::restore_results(const ReadBatch& batch) const {
  std::vector<GetProductResult> results;
  results.reserve(batch.input_positions.size());
  for (const auto position : batch.input_positions) {
    const auto& item = batch.work_items[position];
    if (item.result) {
      results.push_back(*item.result);
    } else {
      results.push_back({ProductStatus::InternalError, std::nullopt, item.source});
    }
  }
  return results;
}

UpdateProductResult ProductService::update(const UpdateProductRequest& request) {
  _shared.metrics.increment(ProductMetric::UpdateRequests);

  if (!valid_update_request(request)) {
    return {ProductStatus::InvalidArgument, std::nullopt, false};
  }
  // 先让数据库完成版本检查和事务提交；在结果明确前不修改缓存。
  StoreUpdateResult store_result;
  try {
    store_result = _store.update(request);
  } catch (const StoreError& error) {
    return {status_from_store_error(error.code()), std::nullopt, false};
  }
  if (store_result.status == StoreUpdateStatus::NotFound) {
    return {ProductStatus::NotFound, std::nullopt, false};
  }
  if (store_result.status == StoreUpdateStatus::Conflict) {
    return {ProductStatus::Conflict, std::nullopt, false};
  }
  if (store_result.status != StoreUpdateStatus::Updated) {
    return {ProductStatus::InternalError, std::nullopt, false};
  }
  // 提交后删除缓存。删除失败只影响读到旧值的风险，不撤销已提交的数据库更新。
  bool invalidation_failed = false;
  try {
    _cache.erase(make_product_cache_key(request.id));
  } catch (const CacheError&) {
    invalidation_failed = true;
    _shared.metrics.increment(ProductMetric::CacheInvalidationFailures);
  }
  if (!store_result.product || !valid_product(*store_result.product) ||
      store_result.product->id != request.id ||
      store_result.product->version != request.expected_version + 1) {
    return {ProductStatus::InternalError, std::nullopt, invalidation_failed};
  }
  return {ProductStatus::Ok, std::move(store_result.product), invalidation_failed};
}

}  // namespace sphinx
