// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <httplib.h>
#include <application/product_shared_state.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace sphinx {

struct ProductHttpConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 8080;
  std::uint32_t worker_count = 4;
};

struct ProductRuntimeConfig;

/// config/shared 必须长于服务；每个 HTTP 线程持有并销毁自己的 WorkerContext。
class ProductHttpServer final {
 public:
  ProductHttpServer(const ProductRuntimeConfig& config, ProductSharedState& shared);
  ~ProductHttpServer();
  ProductHttpServer(const ProductHttpServer&) = delete;
  ProductHttpServer& operator=(const ProductHttpServer&) = delete;

  /// 阻塞运行直至 stop()；绑定或监听失败时返回 false；只能调用一次。
  bool serve();
  /// 控制线程可安全调用，用来结束 serve()；不能从信号处理函数直接调用。
  void stop() noexcept;

 private:
  const ProductRuntimeConfig& _config;
  ProductSharedState& _shared;
  httplib::Server _server;
  std::mutex _state_mutex;
  bool _stopping = false;
  bool _serve_called = false;
};

}  // namespace sphinx
