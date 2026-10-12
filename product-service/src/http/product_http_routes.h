// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <httplib.h>
#include <sphinx/product/application/product_service.h>
#include <sphinx/product/application/protection/product_shared_state.h>

#include <string>

namespace sphinx {

struct ProductRuntimeConfig;

void install_product_routes(httplib::Server& server, const ProductSharedState& shared,
                            const ProductRuntimeConfig& config);

}  // namespace sphinx
