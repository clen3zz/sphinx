// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>
#include <sphinx/product_service.h>

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

}  // namespace

struct ProductService::ReadWorkItem {
  std::uint64_t id = 0;
  std::string key;
  CacheSource source = CacheSource::NotChecked;
  std::optional<GetProductResult> result;
};

struct ProductService::ReadBatch {
  std::vector<ReadWorkItem> work_items;
  std::vector<std::size_t> input_positions;
  bool bypass_cache = false;
  bool cache_failed = false;
};

ProductService::ProductService(ProductStore& store, ProductCache& cache, ProductCachePolicy policy)
    : _store{store}, _cache{cache}, _policy{policy} {
  if (!valid_product_cache_ttl(_policy.ttl_seconds)) {
    throw std::invalid_argument{"product cache TTL must be in 1..30 days"};
  }
}

GetProductResult ProductService::get(std::uint64_t id, bool bypass_cache) {
  if (id == 0) {
    return {ProductStatus::InvalidArgument, std::nullopt, CacheSource::NotChecked};
  }
  auto results = read_products({id}, bypass_cache);
  return std::move(results.front());
}

GetProductsResult ProductService::get_many(const std::vector<std::uint64_t>& ids,
                                           bool bypass_cache) {
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
  std::vector<std::size_t> positions(batch.work_items.size());
  for (std::size_t index = 0; index < positions.size(); ++index) {
    positions[index] = index;
  }

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
  load_basic(batch, unresolved);
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
      batch.work_items.push_back({id, make_product_cache_key(id),
                                  bypass_cache ? CacheSource::Bypass : CacheSource::Miss,
                                  std::nullopt});
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

  try {
    std::vector<std::optional<std::string>> values;
    if (batch.work_items.size() == 1) {
      values.push_back(_cache.get(keys.front()));
    } else {
      values = _cache.get_many(keys);
    }
    if (values.size() != positions.size()) {
      throw CacheError{"cache batch returned an unexpected result count"};
    }

    for (std::size_t index = 0; index < positions.size(); ++index) {
      auto& item = batch.work_items[positions[index]];
      if (!values[index]) {
        item.source = CacheSource::Miss;
        continue;
      }
      const auto cached_value = values[index].value_or("");
      auto product = decode_product_cache(cached_value);
      if (product && product->id == item.id) {
        item.source = CacheSource::Hit;
        item.result = GetProductResult{ProductStatus::Ok, std::move(product), item.source};
        continue;
      }
      erase_corrupt(batch, positions[index]);
    }
  } catch (const CacheError&) {
    batch.cache_failed = true;
    for (const auto position : positions) {
      if (!batch.work_items[position].result) {
        batch.work_items[position].source = CacheSource::Bypass;
      }
    }
  }
}

void ProductService::erase_corrupt(ReadBatch& batch, std::size_t position) {
  auto& item = batch.work_items[position];
  item.source = CacheSource::Corrupt;
  try {
    _cache.erase(item.key);
    // 损坏值无法删除时仍继续查询权威数据库。
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const CacheError&) {
    // Cache invalidation is best-effort.
  }
}

void ProductService::load_basic(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  if (positions.empty()) {
    return;
  }

  std::vector<std::uint64_t> ids;
  ids.reserve(positions.size());
  for (const auto position : positions) {
    ids.push_back(batch.work_items[position].id);
  }
  const auto loaded = load_from_store(ids, batch.work_items.size() > 1);
  for (std::size_t index = 0; index < positions.size(); ++index) {
    auto& item = batch.work_items[positions[index]];
    const auto& load = loaded[index];
    item.result = GetProductResult{load.status, load.product, item.source};
  }
  fill_cache(batch, positions);
}

std::vector<ProductLoadResult> ProductService::load_from_store(
    const std::vector<std::uint64_t>& ids, bool batch_request) {
  if (ids.empty()) {
    return {};
  }

  std::vector<std::optional<Product>> products;
  try {
    if (!batch_request) {
      products.push_back(_store.find(ids.front()));
    } else {
      products = _store.find_many(ids);
    }
  } catch (const StoreError& error) {
    return std::vector<ProductLoadResult>(
        ids.size(), ProductLoadResult{status_from_store_error(error.code()), std::nullopt});
  }

  if (products.size() != ids.size()) {
    return std::vector<ProductLoadResult>(
        ids.size(), ProductLoadResult{ProductStatus::InternalError, std::nullopt});
  }

  std::vector<ProductLoadResult> results;
  results.reserve(ids.size());
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (!products[index]) {
      results.push_back({ProductStatus::NotFound, std::nullopt});
      continue;
    }
    Product product = products[index].value_or(Product{});
    if (!valid_product(product) || product.id != ids[index]) {
      results.push_back({ProductStatus::InternalError, std::nullopt});
      continue;
    }
    results.push_back({ProductStatus::Ok, std::move(product)});
  }
  return results;
}

void ProductService::fill_cache(ReadBatch& batch, const std::vector<std::size_t>& positions) {
  if (batch.bypass_cache) {
    return;
  }

  std::vector<CacheWriteEntry> entries;
  entries.reserve(positions.size());
  for (const auto position : positions) {
    const auto& item = batch.work_items[position];
    if (item.result && item.result->status == ProductStatus::Ok && item.result->product) {
      entries.push_back(
          {item.key, encode_product_cache(*item.result->product), _policy.ttl_seconds});
    }
  }
  if (entries.empty()) {
    return;
  }

  try {
    if (batch.work_items.size() == 1) {
      const auto& entry = entries.front();
      _cache.put(entry.key, entry.value, entry.ttl_seconds);
    } else {
      _cache.put_many(entries);
    }
    // 回填只是加速后续查询，不改变本次商品结果。
    // NOLINTNEXTLINE(bugprone-empty-catch)
  } catch (const CacheError&) {
    // Cache write failures do not change the authoritative result.
  }
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
  }
  if (!store_result.product || !valid_product(*store_result.product) ||
      store_result.product->id != request.id ||
      store_result.product->version != request.expected_version + 1) {
    return {ProductStatus::InternalError, std::nullopt, invalidation_failed};
  }
  return {ProductStatus::Ok, std::move(store_result.product), invalidation_failed};
}

}  // namespace sphinx
