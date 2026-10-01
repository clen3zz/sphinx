// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/bootstrap/product_config.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sphinx {

namespace {

std::string optional_environment_value(const char* name, const char* default_value) {
  // 仅在启动阶段读取环境变量，此时工作线程尚未创建。
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{default_value} : std::string{value};
}

std::string required_environment_value(const char* name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): 配置读取发生在工作线程创建前。
  const char* value = std::getenv(name);
  if (value == nullptr) {
    throw std::invalid_argument{"required service configuration is missing"};
  }
  return std::string{value};
}

std::uint64_t unsigned_environment_value(const char* name, std::uint64_t default_value,
                                         std::uint64_t minimum, std::uint64_t maximum) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): 配置读取发生在工作线程创建前。
  const char* raw_value = std::getenv(name);
  if (raw_value == nullptr) {
    return default_value;
  }
  const std::string_view text{raw_value};
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < minimum || value > maximum) {
    throw std::invalid_argument{"numeric service configuration is invalid"};
  }
  return value;
}

}  // namespace

ProductRuntimeConfig load_product_config() {
  ProductRuntimeConfig config;
  config.mysql.user = required_environment_value("SPHINX_MYSQL_USER");
  config.mysql.password = required_environment_value("SPHINX_MYSQL_PASSWORD");
  config.mysql.database = required_environment_value("SPHINX_MYSQL_DATABASE");
  config.mysql.host = optional_environment_value("SPHINX_MYSQL_HOST", "127.0.0.1");
  config.mysql.port =
      static_cast<std::uint16_t>(unsigned_environment_value("SPHINX_MYSQL_PORT", 3306, 1, 65535));
  config.cache.backend =
      parse_cache_backend(optional_environment_value("SPHINX_CACHE_BACKEND", "sphinx"));
  config.cache_policy.mode =
      parse_cache_policy_mode(optional_environment_value("SPHINX_CACHE_POLICY", "basic"));
  const auto cache_timeout_ms =
      unsigned_environment_value("SPHINX_CACHE_TIMEOUT_MS", 200, 1, 10000);
  const auto cache_timeout =
      std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(cache_timeout_ms)};
  if (config.cache.backend == CacheBackend::Sphinx) {
    config.cache.sphinx.nodes = optional_environment_value("SPHINX_CACHE_NODES", "127.0.0.1:11211");
    config.cache.sphinx.timeout = cache_timeout;
  } else {
    config.cache.redis.host = optional_environment_value("SPHINX_REDIS_HOST", "127.0.0.1");
    config.cache.redis.port =
        static_cast<std::uint16_t>(unsigned_environment_value("SPHINX_REDIS_PORT", 6379, 1, 65535));
    config.cache.redis.database =
        static_cast<std::uint32_t>(unsigned_environment_value("SPHINX_REDIS_DATABASE", 0, 0, 15));
    config.cache.redis.username = optional_environment_value("SPHINX_REDIS_USERNAME", "");
    config.cache.redis.password = optional_environment_value("SPHINX_REDIS_PASSWORD", "");
    config.cache.redis.connect_timeout = cache_timeout;
    config.cache.redis.io_timeout = cache_timeout;
  }
  config.http.bind_address = optional_environment_value("SPHINX_HTTP_BIND", "127.0.0.1");
  config.http.port =
      static_cast<std::uint16_t>(unsigned_environment_value("SPHINX_HTTP_PORT", 8080, 1, 65535));
  config.http.worker_count =
      static_cast<std::uint32_t>(unsigned_environment_value("SPHINX_HTTP_WORKERS", 4, 1, 64));
  config.read_options.max_concurrent_loads = std::min(2U, config.http.worker_count);
  if (config.cache_policy.mode == CachePolicyMode::Protected) {
    config.read_options.max_inflight_keys = static_cast<std::size_t>(
        unsigned_environment_value("SPHINX_READ_MAX_INFLIGHT_KEYS", 1024, 1, 65536));
    config.read_options.max_concurrent_loads = static_cast<std::size_t>(unsigned_environment_value(
        "SPHINX_READ_MAX_CONCURRENT_LOADS", std::min(2U, config.http.worker_count), 1,
        config.http.worker_count));
    config.read_options.wait_timeout =
        std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
            unsigned_environment_value("SPHINX_READ_WAIT_TIMEOUT_MS", 500, 1, 10000))};
    config.breaker_options.failure_threshold = static_cast<std::uint32_t>(
        unsigned_environment_value("SPHINX_CACHE_FAILURE_THRESHOLD", 3, 1, 100));
    config.breaker_options.open_interval =
        std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
            unsigned_environment_value("SPHINX_CACHE_OPEN_INTERVAL_MS", 2000, 1, 60000))};
  }
  config.cache_policy.ttl_seconds = static_cast<std::uint32_t>(
      unsigned_environment_value("SPHINX_CACHE_TTL_SECONDS", 30, 1, max_product_cache_ttl_seconds));
  if (config.cache_policy.mode == CachePolicyMode::Protected) {
    config.cache_policy.negative_ttl_seconds = static_cast<std::uint32_t>(
        unsigned_environment_value("SPHINX_NEGATIVE_TTL_SECONDS", 5, 1, 30));
    config.cache_policy.ttl_jitter_seconds = static_cast<std::uint32_t>(
        unsigned_environment_value("SPHINX_TTL_JITTER_SECONDS", 3, 0, 30));
  }
  validate_product_cache_policy(config.cache_policy);
  validate_product_cache_options(config.cache);
  validate_mysql_options(config.mysql);
  config.http.cache_backend = config.cache.backend == CacheBackend::Redis ? "redis" : "sphinx";
  config.http.cache_policy =
      config.cache_policy.mode == CachePolicyMode::Protected ? "protected" : "basic";
  return config;
}

}  // namespace sphinx
