// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
#include <sphinx/product_http.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

sphinx::ProductHttpConfig load_config() {
  sphinx::ProductHttpConfig config;
  config.mysql.user = required_environment_value("SPHINX_MYSQL_USER");
  config.mysql.password = required_environment_value("SPHINX_MYSQL_PASSWORD");
  config.mysql.database = required_environment_value("SPHINX_MYSQL_DATABASE");
  if (config.mysql.user.empty() || config.mysql.database.empty()) {
    throw std::invalid_argument{"required service configuration is empty"};
  }

  config.mysql.host = optional_environment_value("SPHINX_MYSQL_HOST", "127.0.0.1");
  config.mysql.port =
      static_cast<std::uint16_t>(unsigned_environment_value("SPHINX_MYSQL_PORT", 3306, 1, 65535));
  config.cache.backend =
      sphinx::parse_cache_backend(optional_environment_value("SPHINX_CACHE_BACKEND", "sphinx"));
  config.cache_policy.mode =
      sphinx::parse_cache_policy_mode(optional_environment_value("SPHINX_CACHE_POLICY", "basic"));
  const auto cache_timeout_ms =
      unsigned_environment_value("SPHINX_CACHE_TIMEOUT_MS", 200, 1, 10000);
  const auto cache_timeout =
      std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(cache_timeout_ms)};
  if (config.cache.backend == sphinx::CacheBackend::Sphinx) {
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
  config.bind_address = optional_environment_value("SPHINX_HTTP_BIND", "127.0.0.1");
  config.port =
      static_cast<std::uint16_t>(unsigned_environment_value("SPHINX_HTTP_PORT", 8080, 1, 65535));
  config.worker_count =
      static_cast<std::uint32_t>(unsigned_environment_value("SPHINX_HTTP_WORKERS", 4, 1, 64));
  config.cache_policy.ttl_seconds = static_cast<std::uint32_t>(unsigned_environment_value(
      "SPHINX_CACHE_TTL_SECONDS", 30, 1, sphinx::max_product_cache_ttl_seconds));
  if (config.cache_policy.mode == sphinx::CachePolicyMode::Protected) {
    config.cache_policy.negative_ttl_seconds = static_cast<std::uint32_t>(
        unsigned_environment_value("SPHINX_NEGATIVE_TTL_SECONDS", 5, 1, 30));
    config.cache_policy.ttl_jitter_seconds = static_cast<std::uint32_t>(
        unsigned_environment_value("SPHINX_TTL_JITTER_SECONDS", 3, 0, 30));
  }
  return config;
}

bool block_shutdown_signals(sigset_t* wait_set, sigset_t* previous_mask) noexcept {
  if (sigemptyset(wait_set) != 0 || sigaddset(wait_set, SIGINT) != 0 ||
      sigaddset(wait_set, SIGTERM) != 0 || sigaddset(wait_set, SIGUSR1) != 0) {
    return false;
  }
  return pthread_sigmask(SIG_BLOCK, wait_set, previous_mask) == 0;
}

bool wake_control_thread(std::thread& control_thread) noexcept {
  // SIGUSR1 只用于通知控制线程：serve() 已结束，可以退出等待。
  // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread)
  const int thread_result = pthread_kill(control_thread.native_handle(), SIGUSR1);
  if (thread_result == 0 || thread_result == ESRCH) {
    return true;
  }
  if (kill(getpid(), SIGUSR1) == 0) {
    return false;
  }
  (void)pthread_cancel(control_thread.native_handle());
  return false;
}

bool serve_until_shutdown(sphinx::ProductHttpServer& server, const sigset_t& shutdown_signals) {
  std::atomic<bool> runtime_failed{false};
  std::thread control_thread{[&] {
    int received_signal = 0;
    if (sigwait(&shutdown_signals, &received_signal) != 0) {
      runtime_failed.store(true, std::memory_order_release);
      server.stop();
    } else if (received_signal != SIGUSR1) {
      server.stop();
    }
  }};

  bool serve_result = false;
  try {
    serve_result = server.serve();
  } catch (...) {
    runtime_failed.store(true, std::memory_order_release);
  }

  const bool control_woken = wake_control_thread(control_thread);
  control_thread.join();
  return serve_result && !runtime_failed.load(std::memory_order_acquire) && control_woken;
}

}  // namespace

int main() {
  sigset_t shutdown_signals;
  sigset_t previous_mask;
  if (!block_shutdown_signals(&shutdown_signals, &previous_mask)) {
    std::cerr << "product service signal setup failed\n";
    return 1;
  }

  try {
    auto config = load_config();
    sphinx::ProductHttpServer server{std::move(config)};
    const bool served = serve_until_shutdown(server, shutdown_signals);
    const int restore_result = pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    if (restore_result != 0) {
      std::cerr << "product service signal restore failed\n";
      return 1;
    }
    if (!served) {
      std::cerr << "product service bind, listen or runtime failed\n";
      return 1;
    }
    return 0;
  } catch (...) {
    (void)pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    std::cerr << "product service startup failed\n";
    return 1;
  }
}
