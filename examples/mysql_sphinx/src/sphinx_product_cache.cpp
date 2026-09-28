// SPDX-License-Identifier: Apache-2.0
#include <sphinx/sphinx_product_cache.h>

#include <stdexcept>

namespace sphinx {

SphinxProductCache::SphinxProductCache(std::string_view nodes, std::chrono::milliseconds timeout)
    : _client{nodes, timeout} {}

std::optional<std::string> SphinxProductCache::get(std::string_view key) {
  try {
    return _client.get(key);
  } catch (const ClientError& error) {
    throw CacheError{error.what()};
  }
}

void SphinxProductCache::put(std::string_view key, std::string_view value,
                             std::uint32_t ttl_seconds) {
  if (ttl_seconds == 0 || ttl_seconds > 60U * 60U * 24U * 30U) {
    throw std::invalid_argument{"cache TTL must be in 1..30 days"};
  }
  try {
    if (!_client.set(key, value, ttl_seconds)) {
      throw CacheError{"cache rejected set"};
    }
  } catch (const ClientError& error) {
    throw CacheError{error.what()};
  }
}

void SphinxProductCache::erase(std::string_view key) {
  try {
    _client.remove_status(key);
  } catch (const ClientError& error) {
    throw CacheError{error.what()};
  }
}

}  // namespace sphinx
