// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_cache.h>
#include <sphinx/product_cache_options.h>
#include <sphinx/product_store.h>

#include <vector>

namespace sphinx {

struct ProductCachePolicy {
  CachePolicyMode mode = CachePolicyMode::Basic;
  /// 必须在 1..2,592,000 秒内；更大的过期值会被 Sphinx 解释为 UNIX 时间戳。
  std::uint32_t ttl_seconds = 30;
  std::uint32_t negative_ttl_seconds = 5;
  std::uint32_t ttl_jitter_seconds = 3;
};

/// 旁路缓存业务层：只借用 store 和 cache，二者的生命周期必须长于本对象。
/// 三个对象都由同一个工作线程使用，因此这一层不加锁。
class ProductService final {
 public:
  ProductService(ProductStore& store, ProductCache& cache, ProductCachePolicy policy = {});

  /// 查询：先读缓存并解码、校验 id；未命中或缓存出错时查数据库，再尽力回填。
  /// bypass_cache=true 时直接查权威数据库，用于核实提交结果；不存在的商品不缓存。
  /// 命中的缓存值仍可能是旧版本，直至其 TTL 到期。
  GetProductResult get(std::uint64_t id, bool bypass_cache = false);

  /// 批量读取：结果按输入顺序恢复，重复 ID 返回重复项。
  GetProductsResult get_many(const std::vector<std::uint64_t>& ids, bool bypass_cache = false);

  /// 更新：先完成数据库事务并确认提交，再尽力删除缓存；提交前不能改动缓存。
  /// 版本冲突是正常业务结果；CommitUnknown 时不能删除缓存或盲目重试。
  UpdateProductResult update(const UpdateProductRequest& request);

 private:
  struct ReadWorkItem;
  struct ReadBatch;

  std::vector<GetProductResult> read_products(const std::vector<std::uint64_t>& ids,
                                              bool bypass_cache);
  ReadBatch make_read_batch(const std::vector<std::uint64_t>& ids, bool bypass_cache) const;
  void read_cache(ReadBatch& batch, const std::vector<std::size_t>& positions);
  void erase_corrupt(ReadBatch& batch, std::size_t position);
  void load_basic(ReadBatch& batch, const std::vector<std::size_t>& positions);
  std::vector<ProductLoadResult> load_from_store(const std::vector<std::uint64_t>& ids,
                                                 bool batch_request);
  void fill_cache(ReadBatch& batch, const std::vector<std::size_t>& positions);
  std::vector<GetProductResult> restore_results(const ReadBatch& batch) const;

  ProductStore& _store;
  ProductCache& _cache;
  ProductCachePolicy _policy;
};

}  // namespace sphinx
