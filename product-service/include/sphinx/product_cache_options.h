// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_cache.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace sphinx {

enum class CacheBackend : std::uint8_t { Sphinx, Redis };
enum class CachePolicyMode : std::uint8_t { Basic, Protected };

struct SphinxCacheOptions {
  std::string nodes = "127.0.0.1:11211";
  std::chrono::milliseconds timeout{200};
};

struct RedisOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;
  std::uint32_t database = 0;
  std::string username;
  std::string password;
  std::chrono::milliseconds connect_timeout{200};
  std::chrono::milliseconds io_timeout{200};
};

struct ProductCacheOptions {
  CacheBackend backend = CacheBackend::Sphinx;
  SphinxCacheOptions sphinx;
  RedisOptions redis;
};

CacheBackend parse_cache_backend(std::string_view text);
CachePolicyMode parse_cache_policy_mode(std::string_view text);
void validate_redis_options(const RedisOptions& options);
void validate_product_cache_options(const ProductCacheOptions& options);
std::unique_ptr<ProductCache> make_product_cache(const ProductCacheOptions& options);

}  // namespace sphinx
