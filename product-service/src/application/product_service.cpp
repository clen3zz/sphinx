// SPDX-License-Identifier: Apache-2.0
#include <application/product_codec.h>
#include <application/product_service.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace sphinx {
namespace {

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

struct ProductService::ReadWorkItem {
  std::uint64_t id = 0;
  std::string key;
  CacheSource source = CacheSource::NotChecked;
  std::optional<ProductLoadResult> result;
  ProductReadTicket ticket;
};

struct ProductService::ReadBatch {
  WorkItems pending(const WorkItems& items) const {
    WorkItems result;
    for (auto* item : items) {
      if (!item->result) {
        result.push_back(item);
      }
    }
    return result;
  }
  // 哈希节点保证地址稳定；重复输入可以引用同一个工作项。
  std::unordered_map<std::uint64_t, ReadWorkItem> storage;
  WorkItems items;
  WorkItems input;
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
  _shared.metrics.increment(ProductMetric::RequestUniqueIds, batch.items.size());
  const auto& items = batch.items;

  if (!bypass_cache) {
    read_cache(batch, items);
  }
  const auto unresolved = batch.pending(items);
  if (_policy.mode == CachePolicyMode::Protected) {
    load_protected(batch, unresolved);
  } else {
    load_batch(batch, unresolved);
  }
  // 正常返回前，每个工作项都已由缓存、回源或跟随等待设置结果。
  return restore_results(batch);
}

ProductService::ReadBatch ProductService::make_read_batch(const std::vector<std::uint64_t>& ids,
                                                          bool bypass_cache) const {
  ReadBatch batch;
  batch.bypass_cache = bypass_cache;
  batch.storage.reserve(ids.size());
  for (auto id : ids) {
    auto [position, inserted] = batch.storage.try_emplace(id);
    auto* item = &position->second;
    if (inserted) {
      *item = ReadWorkItem{id,
                           make_product_cache_key(id),
                           bypass_cache ? CacheSource::Bypass : CacheSource::Miss,
                           {},
                           {}};
      batch.items.push_back(item);
    }
    batch.input.push_back(item);
  }
  return batch;
}

void ProductService::read_cache(ReadBatch& batch, const WorkItems& items) {
  if (items.empty()) {
    return;
  }

  std::vector<std::string> keys;
  keys.reserve(items.size());
  for (const auto* item : items) {
    keys.push_back(item->key);
  }

  std::vector<std::optional<std::string>> values;
  const bool succeeded =
      run_cache_operation(_shared, _policy.mode, ProductMetric::CacheReadFailures, [&] {
        _shared.metrics.increment(ProductMetric::CacheLookupKeys, items.size());
        if (batch.items.size() == 1) {
          values.push_back(_cache.get(keys.front()));
        } else {
          values = _cache.get_many(keys);
        }
        if (values.size() != items.size()) {
          throw CacheError{"cache batch returned an unexpected result count"};
        }
      });
  if (!succeeded) {
    batch.cache_failed = true;
    for (auto* item : items) {
      item->source = CacheSource::Bypass;
    }
    return;
  }

  for (std::size_t index = 0; index < items.size(); ++index) {
    auto& item = *items[index];
    const auto& value = values[index];
    if (!value) {
      _shared.metrics.increment(ProductMetric::CacheMisses);
      item.source = CacheSource::Miss;
      continue;
    }
    auto entry = decode_product_cache_entry(*value, item.id);
    if (entry.kind == CacheEntryKind::Product) {
      _shared.metrics.increment(ProductMetric::CacheHits);
      item.source = CacheSource::Hit;
      item.result = ProductLoadResult{ProductStatus::Ok, std::move(entry.product)};
      continue;
    }
    if (entry.kind == CacheEntryKind::NotFound) {
      if (_policy.mode == CachePolicyMode::Protected) {
        _shared.metrics.increment(ProductMetric::NegativeHits);
        item.source = CacheSource::Hit;
        item.result = ProductLoadResult{ProductStatus::NotFound, std::nullopt};
      } else {
        _shared.metrics.increment(ProductMetric::CacheMisses);
        item.source = CacheSource::Miss;
      }
      continue;
    }
    _shared.metrics.increment(ProductMetric::CacheCorrupt);
    erase_corrupt(batch, item);
  }
}

void ProductService::erase_corrupt(ReadBatch& batch, ReadWorkItem& item) {
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

void ProductService::load_batch(ReadBatch& batch, const WorkItems& items) {
  if (items.empty()) {
    return;
  }
  {
    std::optional<ProductLoadPermit> permit;
    if (_policy.mode == CachePolicyMode::Protected) {
      permit = _shared.reads.try_acquire_load();
    }
    if (_policy.mode == CachePolicyMode::Protected && !permit) {
      _shared.metrics.increment(ProductMetric::ReadAdmissionRejected);
      for (auto* item : items) {
        item->result = ProductLoadResult{ProductStatus::ReadBusy, std::nullopt};
      }
      return;
    }

    load_from_store(batch, items);
  }
  // 数据库名额只覆盖回源；缓存 I/O 不占用名额。
  fill_cache(batch, items);
}

void ProductService::load_protected(ReadBatch& batch, const WorkItems& items) {
  if (items.empty()) {
    return;
  }
  if (batch.bypass_cache) {
    load_batch(batch, items);
    return;
  }

  std::vector<std::uint64_t> ids;
  for (const auto* item : items) {
    ids.push_back(item->id);
  }
  auto tickets = _shared.reads.acquire_many(ids);
  WorkItems leaders;
  WorkItems followers;
  leaders.reserve(items.size());
  followers.reserve(items.size());

  for (std::size_t index = 0; index < tickets.size(); ++index) {
    auto& item = *items[index];
    item.ticket = std::move(tickets[index]);
    switch (item.ticket.role()) {
      case ReadRole::Leader:
        _shared.metrics.increment(ProductMetric::ReadLeaders);
        leaders.push_back(&item);
        break;
      case ReadRole::Follower:
        _shared.metrics.increment(ProductMetric::ReadFollowers);
        followers.push_back(&item);
        break;
      case ReadRole::Rejected: {
        _shared.metrics.increment(ProductMetric::ReadRejected);
        item.result = ProductLoadResult{ProductStatus::ReadBusy, std::nullopt};
        break;
      }
    }
  }

  if (!batch.cache_failed) {
    read_cache(batch, leaders);
  }

  load_batch(batch, batch.pending(leaders));

  for (auto* leader : leaders) {
    auto& item = *leader;
    assert(item.result);
    item.ticket.complete(*item.result);
  }

  const auto deadline = std::chrono::steady_clock::now() + _shared.reads.wait_timeout();
  for (auto* follower : followers) {
    auto& item = *follower;
    item.result = item.ticket.wait_until(deadline);
    if (item.ticket.wait_timed_out()) {
      _shared.metrics.increment(ProductMetric::ReadWaitTimeouts);
    }
  }
}

void ProductService::load_from_store(ReadBatch& batch, const WorkItems& items) {
  std::vector<std::uint64_t> ids;
  for (const auto* item : items) {
    ids.push_back(item->id);
  }

  std::vector<std::optional<Product>> products;
  _shared.metrics.increment(ProductMetric::StoreReadOperations);
  _shared.metrics.increment(ProductMetric::StoreReadIds, ids.size());
  try {
    products = batch.items.size() == 1
                   ? std::vector<std::optional<Product>>{_store.find(ids.front())}
                   : _store.find_many(ids);
    if (products.size() != ids.size()) {
      throw StoreError{StoreErrorCode::Unexpected, "store returned an unexpected result count"};
    }
  } catch (const StoreError& error) {
    _shared.metrics.increment(ProductMetric::StoreReadFailures);
    for (auto* item : items) {
      item->result = ProductLoadResult{status_from_store_error(error.code()), {}};
    }
    return;
  } catch (...) {
    _shared.metrics.increment(ProductMetric::StoreReadFailures);
    throw;
  }

  bool invalid_row = false;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    auto& item = *items[index];
    auto& product = products[index];
    if (!product) {
      item.result = ProductLoadResult{ProductStatus::NotFound, std::nullopt};
    } else if (!valid_product(*product) || product->id != item.id) {
      invalid_row = true;
      item.result = ProductLoadResult{ProductStatus::InternalError, std::nullopt};
    } else {
      item.result = ProductLoadResult{ProductStatus::Ok, std::move(product)};
    }
  }
  if (invalid_row) {
    _shared.metrics.increment(ProductMetric::StoreReadFailures);
  }
}

void ProductService::fill_cache(ReadBatch& batch, const WorkItems& items) {
  if (_policy.mode == CachePolicyMode::Protected && batch.cache_failed) {
    return;
  }

  std::vector<CacheWriteEntry> entries;
  entries.reserve(items.size());
  for (const auto* work_item : items) {
    const auto& item = *work_item;
    assert(item.result);
    if (item.result->status == ProductStatus::Ok) {
      assert(item.result->product);
      entries.push_back({item.key, encode_product_cache(*item.result->product),
                         product_cache_ttl(item.id, _policy)});
    } else if (_policy.mode == CachePolicyMode::Protected &&
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
    if (batch.items.size() == 1) {
      const auto& entry = entries.front();
      _cache.put(entry.key, entry.value, entry.ttl_seconds);
    } else {
      _cache.put_many(entries);
    }
  });
}

std::vector<GetProductResult> ProductService::restore_results(const ReadBatch& batch) const {
  std::vector<GetProductResult> results;
  results.reserve(batch.input.size());
  for (const auto* work_item : batch.input) {
    const auto& item = *work_item;
    assert(item.result);
    results.push_back({item.result->status, item.result->product, item.source});
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
