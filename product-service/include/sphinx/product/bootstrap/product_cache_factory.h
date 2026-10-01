// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/application/ports/product_cache.h>
#include <sphinx/product/backends/redis/redis_options.h>
#include <sphinx/product/backends/sphinx/sphinx_cache_options.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace sphinx {

enum class CacheBackend : std::uint8_t { Sphinx, Redis };

struct ProductCacheOptions {
  CacheBackend backend = CacheBackend::Sphinx;
  SphinxCacheOptions sphinx;
  RedisOptions redis;
};

CacheBackend parse_cache_backend(std::string_view text);
void validate_product_cache_options(const ProductCacheOptions& options);
std::unique_ptr<ProductCache> make_product_cache(const ProductCacheOptions& options);

}  // namespace sphinx
