// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <httplib.h>
#include <sphinx/product_service.h>
#include <sphinx/product_shared_state.h>

#include <functional>

namespace sphinx {

/// 内部接口：回调返回当前 HTTP 工作线程持有的 ProductService。
/// 线程初始化可能抛出 StoreError；路由将 Unavailable 映射为 503，
/// 其他初始化错误返回不泄露内部细节的 500 响应。
void install_product_routes(httplib::Server& server,
                            const std::function<ProductService&()>& current_service,
                            const ProductSharedState& shared, CacheBackend backend,
                            CachePolicyMode policy_mode);

}  // namespace sphinx
