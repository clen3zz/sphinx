// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/cluster_client.h>
#include <sphinx/product_cache.h>

#include <chrono>

namespace sphinx {

/// Owns exactly one ClusterClient; construct/use/destroy on the same worker thread.
class SphinxProductCache final : public ProductCache {
 public:
  explicit SphinxProductCache(std::string_view nodes,
                              std::chrono::milliseconds timeout = ClusterClient::kDefaultTimeout);

  std::optional<std::string> get(std::string_view key) override;
  void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) override;
  void erase(std::string_view key) override;

 private:
  ClusterClient _client;
};

}  // namespace sphinx
