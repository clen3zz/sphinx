// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/application/product_metrics.h>

#include <limits>

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

}  // namespace

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

ProductMetricsSnapshot ProductMetrics::snapshot() const noexcept {
  ProductMetricsSnapshot result;
  for (std::size_t index = 0; index < _counters.size(); ++index) {
    result.counters[index] = _counters[index].load(std::memory_order_relaxed);
  }
  return result;
}

}  // namespace sphinx
