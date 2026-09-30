// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace sphinx {

enum class ProductMetric : std::uint8_t {
  GetRequests,
  BatchRequests,
  UpdateRequests,
  RequestUniqueIds,
  CacheLookupKeys,
  CacheHits,
  NegativeHits,
  CacheMisses,
  CacheCorrupt,
  CacheReadFailures,
  CacheFillFailures,
  CacheInvalidationFailures,
  CacheCleanupFailures,
  StoreReadOperations,
  StoreReadIds,
  StoreReadFailures,
  ReadLeaders,
  ReadFollowers,
  ReadRejected,
  ReadWaitTimeouts,
  ReadAdmissionRejected,
  CacheCircuitBypasses,
  Count,
};

inline constexpr std::size_t product_metric_count = static_cast<std::size_t>(ProductMetric::Count);

enum class ProductLatency : std::uint8_t {
  CacheRead,
  StoreRead,
  CacheFill,
  CacheErase,
  ReadWait,
  GetRequest,
  BatchRequest,
  UpdateRequest,
  Count,
};

inline constexpr std::size_t product_latency_count =
    static_cast<std::size_t>(ProductLatency::Count);

inline constexpr std::array<std::uint64_t, 10> latency_bucket_upper_bounds_microseconds{
    100, 250, 500, 1000, 2500, 5000, 10000, 50000, 200000, 1000000};
inline constexpr std::size_t latency_bucket_count =
    latency_bucket_upper_bounds_microseconds.size() + 1;

struct ProductLatencySnapshot {
  std::array<std::uint64_t, latency_bucket_count> buckets{};
  std::uint64_t count = 0;
  std::uint64_t total_microseconds = 0;
};

struct ProductMetricsSnapshot {
  std::array<std::uint64_t, product_metric_count> counters{};
  std::array<ProductLatencySnapshot, product_latency_count> latencies{};
};

struct ProductLatencyCounters final {
  ProductLatencyCounters() noexcept;
  ProductLatencyCounters(const ProductLatencyCounters&) = delete;
  ProductLatencyCounters& operator=(const ProductLatencyCounters&) = delete;

  std::array<std::atomic<std::uint64_t>, latency_bucket_count> buckets;
  std::atomic<std::uint64_t> count;
  std::atomic<std::uint64_t> total_microseconds;
};

class ProductMetrics final {
 public:
  ProductMetrics() noexcept;
  ProductMetrics(const ProductMetrics&) = delete;
  ProductMetrics& operator=(const ProductMetrics&) = delete;

  void increment(ProductMetric metric, std::uint64_t amount = 1) noexcept;
  void observe(ProductLatency latency, std::chrono::microseconds elapsed) noexcept;
  ProductMetricsSnapshot snapshot() const noexcept;

 private:
  std::array<std::atomic<std::uint64_t>, product_metric_count> _counters;
  std::array<ProductLatencyCounters, product_latency_count> _latencies;
};

class ScopedProductTimer final {
 public:
  ScopedProductTimer(ProductMetrics& metrics, ProductLatency latency) noexcept;
  ~ScopedProductTimer();
  ScopedProductTimer(const ScopedProductTimer&) = delete;
  ScopedProductTimer& operator=(const ScopedProductTimer&) = delete;
  ScopedProductTimer(ScopedProductTimer&&) = delete;
  ScopedProductTimer& operator=(ScopedProductTimer&&) = delete;

 private:
  ProductMetrics& _metrics;
  ProductLatency _latency;
  std::chrono::steady_clock::time_point _started_at;
};

}  // namespace sphinx
