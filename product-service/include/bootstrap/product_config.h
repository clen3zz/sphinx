// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <application/product_cache_policy.h>
#include <backends/mysql/mysql_options.h>
#include <bootstrap/product_cache_factory.h>
#include <http/product_http.h>

namespace sphinx {

struct ProductRuntimeConfig {
  ProductHttpConfig http;
  ProductCacheOptions cache;
  ProductCachePolicy cache_policy;
  ProductReadOptions read_options;
  CacheBreakerOptions breaker_options;
  MySqlOptions mysql;
};

/// 启动阶段读取环境变量并校验，工作线程只读配置。
ProductRuntimeConfig load_product_config();

}  // namespace sphinx
