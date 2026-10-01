// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/backends/sphinx/sphinx_product_cache.h>

#include <stdexcept>

namespace sphinx {

SphinxProductCache::SphinxProductCache(std::string_view nodes, std::chrono::milliseconds timeout)
    : _client{nodes, timeout} {}

std::optional<std::string> SphinxProductCache::get(std::string_view key) {
  // 将集群客户端的传输错误统一转换为缓存错误，业务层才能选择回源 MySQL。
  try {
    return _client.get(key);
  } catch (const ClientError& error) {
    throw CacheError{error.what()};
  }
}

void SphinxProductCache::put(std::string_view key, std::string_view value,
                             std::uint32_t ttl_seconds) {
  if (!valid_product_cache_ttl(ttl_seconds)) {
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
    // 缓存项本来就不存在也算删除成功，适合数据库提交后的失效操作。
    _client.remove_status(key);
  } catch (const ClientError& error) {
    throw CacheError{error.what()};
  }
}

std::vector<std::optional<std::string>> SphinxProductCache::get_many(
    const std::vector<std::string>& keys) {
  validate_product_cache_keys(keys);
  try {
    return _client.get_many(keys);
  } catch (const ClientError& error) {
    throw CacheError{error.what()};
  }
}

}  // namespace sphinx
