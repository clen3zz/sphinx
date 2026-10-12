// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
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

struct ProductMetricsSnapshot {
  std::array<std::uint64_t, product_metric_count> counters{};
};

class ProductMetrics final {
 public:
  ProductMetrics() noexcept;
  ProductMetrics(const ProductMetrics&) = delete;
  ProductMetrics& operator=(const ProductMetrics&) = delete;

  void increment(ProductMetric metric, std::uint64_t amount = 1) noexcept;
  ProductMetricsSnapshot snapshot() const noexcept;

 private:
  std::array<std::atomic<std::uint64_t>, product_metric_count> _counters;
};

}  // namespace sphinx
