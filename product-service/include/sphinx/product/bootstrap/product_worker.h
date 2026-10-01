// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/bootstrap/product_config.h>

namespace sphinx {

/// config 和 shared 必须长于使用此回调的 HTTP 服务。
ProductServiceFactory make_product_worker_factory(const ProductRuntimeConfig& config,
                                                  ProductSharedState& shared);

}  // namespace sphinx
