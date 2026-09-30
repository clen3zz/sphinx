// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_read_coordinator.h>

namespace sphinx {

struct ProductSharedState final {
  explicit ProductSharedState(ProductReadOptions read_options = {}) : reads{read_options} {}
  ProductSharedState(const ProductSharedState&) = delete;
  ProductSharedState& operator=(const ProductSharedState&) = delete;

  ProductReadCoordinator reads;
};

}  // namespace sphinx
