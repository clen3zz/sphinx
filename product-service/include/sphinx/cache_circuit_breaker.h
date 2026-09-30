// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace sphinx {

struct CacheBreakerOptions {
  std::uint32_t failure_threshold = 3;
  std::chrono::milliseconds open_interval{2000};
};

enum class CacheBreakerState : std::uint8_t { Closed, Open, HalfOpen };

struct CacheBreakerSnapshot {
  CacheBreakerState state = CacheBreakerState::Closed;
  std::uint32_t consecutive_failures = 0;
  std::chrono::milliseconds retry_after{0};
};

using CacheNowFunction = std::function<std::chrono::steady_clock::time_point()>;

std::chrono::steady_clock::time_point default_cache_now() noexcept;

class CacheCircuitBreaker;

class CacheOperationPermit final {
 public:
  CacheOperationPermit() = default;
  ~CacheOperationPermit();
  CacheOperationPermit(const CacheOperationPermit&) = delete;
  CacheOperationPermit& operator=(const CacheOperationPermit&) = delete;
  CacheOperationPermit(CacheOperationPermit&& other) noexcept;
  CacheOperationPermit& operator=(CacheOperationPermit&& other) noexcept;

  void succeed() noexcept;
  void fail() noexcept;

 private:
  CacheOperationPermit(CacheCircuitBreaker* owner, std::uint64_t generation, bool probe) noexcept;
  void finish(bool succeeded) noexcept;

  CacheCircuitBreaker* _owner = nullptr;
  std::uint64_t _generation = 0;
  bool _probe = false;

  friend class CacheCircuitBreaker;
};

class CacheCircuitBreaker final {
 public:
  explicit CacheCircuitBreaker(CacheBreakerOptions options,
                               CacheNowFunction now = default_cache_now);
  CacheCircuitBreaker(const CacheCircuitBreaker&) = delete;
  CacheCircuitBreaker& operator=(const CacheCircuitBreaker&) = delete;
  CacheCircuitBreaker(CacheCircuitBreaker&&) = delete;
  CacheCircuitBreaker& operator=(CacheCircuitBreaker&&) = delete;

  std::optional<CacheOperationPermit> try_acquire();
  CacheBreakerSnapshot snapshot() const;

 private:
  void finish(std::uint64_t generation, bool succeeded, bool probe) noexcept;

  CacheBreakerOptions _options;
  CacheNowFunction _now;
  mutable std::mutex _mutex;
  CacheBreakerState _state = CacheBreakerState::Closed;
  std::uint32_t _consecutive_failures = 0;
  std::chrono::steady_clock::time_point _retry_at{};
  std::uint64_t _generation = 0;

  friend class CacheOperationPermit;
};

}  // namespace sphinx
