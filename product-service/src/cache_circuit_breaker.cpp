// SPDX-License-Identifier: Apache-2.0
#include <sphinx/cache_circuit_breaker.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sphinx {
namespace {

constexpr std::uint32_t max_failure_threshold = 100;
constexpr std::chrono::milliseconds max_open_interval{60000};

}  // namespace

std::chrono::steady_clock::time_point default_cache_now() noexcept {
  return std::chrono::steady_clock::now();
}

CacheOperationPermit::CacheOperationPermit(CacheCircuitBreaker* owner,
                                           std::uint64_t generation) noexcept
    : _owner{owner}, _generation{generation} {}

CacheOperationPermit::~CacheOperationPermit() { fail(); }

CacheOperationPermit::CacheOperationPermit(CacheOperationPermit&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)}, _generation{other._generation} {}

CacheOperationPermit& CacheOperationPermit::operator=(CacheOperationPermit&& other) noexcept {
  if (this != &other) {
    fail();
    _owner = std::exchange(other._owner, nullptr);
    _generation = other._generation;
  }
  return *this;
}

void CacheOperationPermit::succeed() noexcept { finish(true); }

void CacheOperationPermit::fail() noexcept { finish(false); }

void CacheOperationPermit::finish(bool succeeded) noexcept {
  if (_owner != nullptr) {
    CacheCircuitBreaker* owner = std::exchange(_owner, nullptr);
    owner->finish(_generation, succeeded);
  }
}

CacheCircuitBreaker::CacheCircuitBreaker(CacheBreakerOptions options, CacheNowFunction now)
    : _options{options}, _now{std::move(now)} {
  if (_options.failure_threshold == 0 || _options.failure_threshold > max_failure_threshold ||
      _options.open_interval.count() <= 0 || _options.open_interval > max_open_interval || !_now) {
    throw std::invalid_argument{"invalid cache circuit breaker options"};
  }
}

std::optional<CacheOperationPermit> CacheCircuitBreaker::try_acquire() {
  std::lock_guard lock{_mutex};
  if (_state == CacheBreakerState::Closed) {
    return CacheOperationPermit{this, _generation};
  }
  if (_state == CacheBreakerState::HalfOpen || _now() < _retry_at) {
    return std::nullopt;
  }

  _state = CacheBreakerState::HalfOpen;
  ++_generation;
  return CacheOperationPermit{this, _generation};
}

CacheBreakerSnapshot CacheCircuitBreaker::snapshot() const {
  std::lock_guard lock{_mutex};
  CacheBreakerSnapshot result{_state, _consecutive_failures, std::chrono::milliseconds{0}};
  if (_state == CacheBreakerState::Open) {
    const auto now = _now();
    if (_retry_at > now) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(_retry_at - now);
      result.retry_after = std::max(remaining, std::chrono::milliseconds{0});
    }
  }
  return result;
}

void CacheCircuitBreaker::finish(std::uint64_t generation, bool succeeded) noexcept {
  std::lock_guard lock{_mutex};
  if (generation != _generation) {
    return;
  }

  // 进入 HalfOpen 会换代，因此当前代的许可只能是唯一的恢复探测。
  if (_state == CacheBreakerState::HalfOpen) {
    if (succeeded) {
      _state = CacheBreakerState::Closed;
      _consecutive_failures = 0;
    } else {
      _state = CacheBreakerState::Open;
      _consecutive_failures = _options.failure_threshold;
      _retry_at = _now() + _options.open_interval;
    }
    ++_generation;
    return;
  }

  if (_state != CacheBreakerState::Closed) {
    return;
  }
  if (succeeded) {
    _consecutive_failures = 0;
    return;
  }

  ++_consecutive_failures;
  if (_consecutive_failures >= _options.failure_threshold) {
    _state = CacheBreakerState::Open;
    _retry_at = _now() + _options.open_interval;
    ++_generation;
  }
}

}  // namespace sphinx
