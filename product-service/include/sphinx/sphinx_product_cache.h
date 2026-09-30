// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/cluster_client.h>
#include <sphinx/product_cache.h>

#include <chrono>

namespace sphinx {

/// 独占一个 ClusterClient；构造、使用和销毁都必须在同一个工作线程中完成。
class SphinxProductCache final : public ProductCache {
 public:
  explicit SphinxProductCache(std::string_view nodes,
                              std::chrono::milliseconds timeout = ClusterClient::kDefaultTimeout);

  std::optional<std::string> get(std::string_view key) override;
  void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) override;
  void erase(std::string_view key) override;
  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) override;
  void put_many(const std::vector<CacheWriteEntry>& entries) override;

 private:
  ClusterClient _client;
};

}  // namespace sphinx
