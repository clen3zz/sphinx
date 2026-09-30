// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_metrics.h>

#include <algorithm>
#include <limits>
#include <type_traits>

namespace sphinx {
namespace {

void add_saturated(std::atomic<std::uint64_t>& target, std::uint64_t amount) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (true) {
    const auto next = amount > std::numeric_limits<std::uint64_t>::max() - current
                          ? std::numeric_limits<std::uint64_t>::max()
                          : current + amount;
    if (target.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                     std::memory_order_relaxed)) {
      return;
    }
  }
}

std::uint64_t nonnegative_microseconds(std::chrono::microseconds elapsed) noexcept {
  using Rep = std::chrono::microseconds::rep;
  static_assert(std::is_integral_v<Rep>);
  const Rep count = elapsed.count();
  if constexpr (std::is_signed_v<Rep>) {
    if (count <= 0) {
      return 0;
    }
  }

  using UnsignedRep = std::make_unsigned_t<Rep>;
  const auto positive = static_cast<UnsignedRep>(count);
  if constexpr (sizeof(UnsignedRep) > sizeof(std::uint64_t)) {
    if (positive > static_cast<UnsignedRep>(std::numeric_limits<std::uint64_t>::max())) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  return static_cast<std::uint64_t>(positive);
}

}  // namespace

ProductLatencyCounters::ProductLatencyCounters() noexcept {
  for (auto& bucket : buckets) {
    bucket.store(0, std::memory_order_relaxed);
  }
  count.store(0, std::memory_order_relaxed);
  total_microseconds.store(0, std::memory_order_relaxed);
}

ProductMetrics::ProductMetrics() noexcept {
  for (auto& counter : _counters) {
    counter.store(0, std::memory_order_relaxed);
  }
}

void ProductMetrics::increment(ProductMetric metric, std::uint64_t amount) noexcept {
  const auto index = static_cast<std::size_t>(metric);
  if (index < _counters.size()) {
    add_saturated(_counters[index], amount);
  }
}

void ProductMetrics::observe(ProductLatency latency, std::chrono::microseconds elapsed) noexcept {
  const auto latency_index = static_cast<std::size_t>(latency);
  if (latency_index >= _latencies.size()) {
    return;
  }

  const auto elapsed_us = nonnegative_microseconds(elapsed);
  auto& counters = _latencies[latency_index];
  std::size_t bucket_index = 0;
  while (bucket_index < latency_bucket_upper_bounds_microseconds.size() &&
         elapsed_us > latency_bucket_upper_bounds_microseconds[bucket_index]) {
    ++bucket_index;
  }
  add_saturated(counters.buckets[bucket_index], 1);
  add_saturated(counters.count, 1);
  add_saturated(counters.total_microseconds, elapsed_us);
}

ProductMetricsSnapshot ProductMetrics::snapshot() const noexcept {
  ProductMetricsSnapshot result;
  for (std::size_t index = 0; index < _counters.size(); ++index) {
    result.counters[index] = _counters[index].load(std::memory_order_relaxed);
  }
  for (std::size_t latency_index = 0; latency_index < _latencies.size(); ++latency_index) {
    const auto& source = _latencies[latency_index];
    auto& target = result.latencies[latency_index];
    for (std::size_t bucket_index = 0; bucket_index < source.buckets.size(); ++bucket_index) {
      target.buckets[bucket_index] = source.buckets[bucket_index].load(std::memory_order_relaxed);
    }
    target.count = source.count.load(std::memory_order_relaxed);
    target.total_microseconds = source.total_microseconds.load(std::memory_order_relaxed);
  }
  return result;
}

ScopedProductTimer::ScopedProductTimer(ProductMetrics& metrics, ProductLatency latency) noexcept
    : _metrics{metrics}, _latency{latency}, _started_at{std::chrono::steady_clock::now()} {}

ScopedProductTimer::~ScopedProductTimer() {
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - _started_at);
  _metrics.observe(_latency, elapsed);
}

}  // namespace sphinx
