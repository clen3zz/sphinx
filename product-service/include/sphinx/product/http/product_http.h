// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/application/product_service.h>
#include <sphinx/product/application/protection/product_shared_state.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sphinx {

struct ProductHttpConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 8080;
  std::uint32_t worker_count = 4;
  // 只作为指标标签，不参与后端选择或连接配置。
  std::string cache_backend = "sphinx";
  std::string cache_policy = "basic";
};

using ProductServiceFactory = std::function<ProductService&()>;

/// 回调须返回当前 Worker 的业务服务，并保持服务及共享状态存活至 HTTP Worker 退出。
class ProductHttpServer final {
 public:
  ProductHttpServer(ProductHttpConfig config, ProductServiceFactory current_service,
                    const ProductSharedState& shared);
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

}  // namespace sphinx
