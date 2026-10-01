// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_metrics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace sphinx {
namespace {

TEST(ProductMetricsTest, CountsEventsByStableEnumIndex) {
  ProductMetrics metrics;
  metrics.increment(ProductMetric::CacheHits);
  metrics.increment(ProductMetric::CacheHits, 3);
  metrics.increment(ProductMetric::Count);

  const auto snapshot = metrics.snapshot();

  EXPECT_EQ(snapshot.counters[static_cast<std::size_t>(ProductMetric::CacheHits)], 4U);
  EXPECT_EQ(snapshot.counters[static_cast<std::size_t>(ProductMetric::CacheMisses)], 0U);
}

TEST(ProductMetricsTest, ConcurrentCounterUpdatesAreNotLost) {
  constexpr std::size_t thread_count = 4;
  constexpr std::uint64_t increments_per_thread = 10000;
  ProductMetrics metrics;
  std::array<std::thread, thread_count> threads;
  for (auto& thread : threads) {
    thread = std::thread{[&metrics] {
      for (std::uint64_t index = 0; index < increments_per_thread; ++index) {
        metrics.increment(ProductMetric::ReadFollowers);
      }
    }};
  }
  for (auto& thread : threads) {
    thread.join();
  }

  const auto snapshot = metrics.snapshot();
  EXPECT_EQ(snapshot.counters[static_cast<std::size_t>(ProductMetric::ReadFollowers)],
            thread_count * increments_per_thread);
}

}  // namespace
}  // namespace sphinx
