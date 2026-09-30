// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_cache.h>
#include <sphinx/product_cache_options.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sphinx {

/// 同步 Redis 客户端；一个实例只允许由构造它的线程使用。
class RedisProductCache final : public ProductCache {
 public:
  explicit RedisProductCache(RedisOptions options);
  ~RedisProductCache() override;
  RedisProductCache(const RedisProductCache&) = delete;
  RedisProductCache& operator=(const RedisProductCache&) = delete;
  RedisProductCache(RedisProductCache&&) = delete;
  RedisProductCache& operator=(RedisProductCache&&) = delete;

  std::optional<std::string> get(std::string_view key) override;
  void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) override;
  void erase(std::string_view key) override;
  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) override;
  void put_many(const std::vector<CacheWriteEntry>& entries) override;

 private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace sphinx
