// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/bootstrap/product_config.h>

namespace sphinx {

/// 在当前 HTTP 工作线程创建资源，工作循环返回后按逆序销毁。
void run_product_worker(const ProductRuntimeConfig& config, ProductSharedState& shared,
                        const ProductWorkerLoop& run_jobs);

}  // namespace sphinx
