// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/bootstrap/product_config.h>

namespace sphinx {

class ProductService;

/// 队列在线程中直接创建 WorkerContext，并在退出时等待资源销毁。
httplib::TaskQueue* make_product_worker_queue(const ProductRuntimeConfig& config,
                                              ProductSharedState& shared, httplib::Server& server);

/// 仅在 HTTP 工作线程处理请求时访问该线程持有的服务。
ProductService& current_product_service();

}  // namespace sphinx
