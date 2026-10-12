// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hiredis.h>
#include <sphinx/product/application/ports/product_cache.h>
#include <sphinx/product/backends/redis/redis_options.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
  struct ContextDeleter {
    void operator()(redisContext* context) const noexcept;
  };
  struct ReplyDeleter {
    void operator()(redisReply* reply) const noexcept;
  };
  using ContextPtr = std::unique_ptr<redisContext, ContextDeleter>;
  using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

  void check_owner() const;
  void ensure_connection();
  void reset_connection() noexcept;
  ReplyPtr command(const std::vector<std::string_view>& arguments);
  void append_command(const std::vector<std::string_view>& arguments);
  ReplyPtr read_reply();
  void expect_reply(const redisReply& reply, bool valid, std::string_view operation);
  void expect_ok(const redisReply& reply, std::string_view operation);
  void expect_startup_ok(const std::vector<std::string_view>& arguments, const char* operation);
  std::optional<std::string> parse_value(const redisReply& reply);

  RedisOptions _options;
  std::thread::id _owner_thread = std::this_thread::get_id();
  ContextPtr _context;
};

}  // namespace sphinx
