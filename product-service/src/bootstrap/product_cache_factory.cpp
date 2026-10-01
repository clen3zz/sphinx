// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/application/ports/product_cache.h>
#include <sphinx/product/backends/redis/redis_product_cache.h>
#include <sphinx/product/backends/sphinx/sphinx_product_cache.h>
#include <sphinx/product/bootstrap/product_cache_factory.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace sphinx {

CacheBackend parse_cache_backend(std::string_view text) {
  if (text == "sphinx") {
    return CacheBackend::Sphinx;
  }
  if (text == "redis") {
    return CacheBackend::Redis;
  }
  throw std::invalid_argument{"cache backend must be sphinx or redis"};
}

void validate_product_cache_options(const ProductCacheOptions& options) {
  switch (options.backend) {
    case CacheBackend::Sphinx:
      validate_sphinx_options(options.sphinx);
      return;
    case CacheBackend::Redis:
      validate_redis_options(options.redis);
      return;
  }
  throw std::invalid_argument{"invalid cache backend"};
}

std::unique_ptr<ProductCache> make_product_cache(const ProductCacheOptions& options) {
  validate_product_cache_options(options);
  switch (options.backend) {
    case CacheBackend::Sphinx:
      return std::make_unique<SphinxProductCache>(options.sphinx.nodes, options.sphinx.timeout);
    case CacheBackend::Redis:
      return std::make_unique<RedisProductCache>(options.redis);
  }
  throw std::invalid_argument{"invalid cache backend"};
}

}  // namespace sphinx
