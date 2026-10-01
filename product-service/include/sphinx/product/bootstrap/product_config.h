// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/backends/mysql/mysql_options.h>
#include <sphinx/product/bootstrap/product_cache_factory.h>
#include <sphinx/product/http/product_http.h>

namespace sphinx {

struct ProductRuntimeConfig {
  ProductHttpConfig http;
  ProductCacheOptions cache;
  ProductCachePolicy cache_policy;
  ProductReadOptions read_options;
  CacheBreakerOptions breaker_options;
  MySqlOptions mysql;
};

/// 启动阶段读取环境变量并校验，工作线程只使用配置副本。
ProductRuntimeConfig load_product_config();

}  // namespace sphinx
