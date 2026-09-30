// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/cache_circuit_breaker.h>
#include <sphinx/product_metrics.h>
#include <sphinx/product_read_coordinator.h>

namespace sphinx {

struct ProductSharedState final {
  explicit ProductSharedState(ProductReadOptions read_options = {},
                              CacheBreakerOptions breaker_options = {})
      : reads{read_options}, breaker{breaker_options} {}
  ProductSharedState(const ProductSharedState&) = delete;
  ProductSharedState& operator=(const ProductSharedState&) = delete;

  ProductMetrics metrics;
  ProductReadCoordinator reads;
  CacheCircuitBreaker breaker;
};

}  // namespace sphinx
