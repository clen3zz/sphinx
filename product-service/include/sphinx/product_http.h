// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/mysql_product_store.h>
#include <sphinx/product_service.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace sphinx {

struct ProductHttpConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 8080;
  std::uint32_t worker_count = 4;
  std::string cache_nodes = "127.0.0.1:11211";
  std::chrono::milliseconds cache_timeout{200};
  ProductCachePolicy cache_policy{};
  MySqlOptions mysql{};
};

/// 持有固定大小的 HTTP 工作线程池。每个线程按顺序创建自己的 MySqlThreadGuard、
/// MySqlProductStore、SphinxProductCache 和 ProductService，销毁顺序相反。
class ProductHttpServer final {
 public:
  explicit ProductHttpServer(ProductHttpConfig config);
  ~ProductHttpServer();
  ProductHttpServer(const ProductHttpServer&) = delete;
  ProductHttpServer& operator=(const ProductHttpServer&) = delete;

  /// 阻塞运行直至 stop()；绑定或监听失败时返回 false；只能调用一次。
  bool serve();
  /// 控制线程可安全调用，用来结束 serve()；不能从信号处理函数直接调用。
  void stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // 命名空间 sphinx
