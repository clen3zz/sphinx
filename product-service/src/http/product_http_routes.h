// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <httplib.h>
#include <sphinx/product/application/product_service.h>
#include <sphinx/product/application/protection/product_shared_state.h>

#include <string>

namespace sphinx {

struct ProductRuntimeConfig;

/// 仅在 HTTP 工作线程处理请求时访问其栈上持有的服务。
ProductService& current_product_service();

void install_product_routes(httplib::Server& server, const ProductSharedState& shared,
                            const ProductRuntimeConfig& config);

}  // namespace sphinx
