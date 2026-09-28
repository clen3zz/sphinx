// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
#include <signal.h>
#include <sphinx/product_http.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
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
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{default_value} : std::string{value};
}

std::string required_environment_value(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    throw std::invalid_argument{"required service configuration is missing"};
  }
  return std::string{value};
}

std::uint64_t unsigned_environment_value(const char* name, std::uint64_t default_value,
                                         std::uint64_t minimum, std::uint64_t maximum) {
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
  config.cache_nodes = optional_environment_value("SPHINX_CACHE_NODES", "127.0.0.1:11211");
  config.bind_address = optional_environment_value("SPHINX_HTTP_BIND", "127.0.0.1");
  config.port =
      static_cast<std::uint16_t>(unsigned_environment_value("SPHINX_HTTP_PORT", 8080, 1, 65535));
  config.worker_count =
      static_cast<std::uint32_t>(unsigned_environment_value("SPHINX_HTTP_WORKERS", 4, 1, 64));
  config.cache_policy.ttl_seconds = static_cast<std::uint32_t>(
      unsigned_environment_value("SPHINX_CACHE_TTL_SECONDS", 30, 1, 2'592'000));
  return config;
}

bool block_shutdown_signals(sigset_t* wait_set, sigset_t* previous_mask) noexcept {
  if (sigemptyset(wait_set) != 0 || sigaddset(wait_set, SIGINT) != 0 ||
      sigaddset(wait_set, SIGTERM) != 0) {
    return false;
  }
  return pthread_sigmask(SIG_BLOCK, wait_set, previous_mask) == 0;
}

bool wake_control_thread(std::thread& control_thread) noexcept {
  const int thread_result = pthread_kill(control_thread.native_handle(), SIGTERM);
  if (thread_result == 0 || thread_result == ESRCH) {
    return true;
  }
  if (kill(getpid(), SIGTERM) == 0) {
    return false;
  }
  (void)pthread_cancel(control_thread.native_handle());
  return false;
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
    std::atomic<bool> done{false};
    std::atomic<bool> control_failed{false};
    std::thread control_thread{[&] {
      int received_signal = 0;
      const int wait_result = sigwait(&shutdown_signals, &received_signal);
      if (wait_result != 0) {
        control_failed.store(true, std::memory_order_release);
        server.stop();
      } else if ((received_signal == SIGINT || received_signal == SIGTERM) &&
                 !done.load(std::memory_order_acquire)) {
        server.stop();
      }
    }};

    bool serve_result = false;
    bool serve_threw = false;
    try {
      serve_result = server.serve();
    } catch (...) {
      serve_threw = true;
    }

    done.store(true, std::memory_order_release);
    const bool control_woken = wake_control_thread(control_thread);
    control_thread.join();
    const int restore_result = pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    if (restore_result != 0) {
      std::cerr << "product service signal restore failed\n";
      return 1;
    }
    if (!serve_result || serve_threw || control_failed.load(std::memory_order_acquire) ||
        !control_woken) {
      std::cerr << (serve_threw ? "product service runtime failed\n"
                                : "product service bind or listen failed\n");
      return 1;
    }
    return 0;
  } catch (...) {
    (void)pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    std::cerr << "product service startup failed\n";
    return 1;
  }
}
