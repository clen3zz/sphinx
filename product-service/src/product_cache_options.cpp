// SPDX-License-Identifier: Apache-2.0
#include <sphinx/cluster.h>
#include <sphinx/product_cache.h>
#include <sphinx/product_cache_options.h>
#include <sphinx/redis_product_cache.h>
#include <sphinx/sphinx_product_cache.h>

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

CachePolicyMode parse_cache_policy_mode(std::string_view text) {
  if (text == "basic") {
    return CachePolicyMode::Basic;
  }
  if (text == "protected") {
    return CachePolicyMode::Protected;
  }
  throw std::invalid_argument{"cache policy must be basic or protected"};
}

void validate_product_cache_options(const ProductCacheOptions& options) {
  constexpr std::chrono::milliseconds max_timeout{10000};
  switch (options.backend) {
    case CacheBackend::Sphinx:
      if (options.sphinx.timeout.count() <= 0 || options.sphinx.timeout > max_timeout) {
        throw std::invalid_argument{"invalid Sphinx cache timeout"};
      }
      (void)parse_nodes(options.sphinx.nodes);
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
