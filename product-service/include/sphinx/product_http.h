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

/// Owns a fixed HTTP worker pool. Each worker owns its own MySqlThreadGuard, MySqlProductStore,
/// SphinxProductCache, and ProductService in that construction/destruction order.
class ProductHttpServer final {
 public:
  explicit ProductHttpServer(ProductHttpConfig config);
  ~ProductHttpServer();
  ProductHttpServer(const ProductHttpServer&) = delete;
  ProductHttpServer& operator=(const ProductHttpServer&) = delete;

  /// Blocks until stop(); returns false if bind/listen fails. Must be called once.
  bool serve();
  /// Thread-safe control-thread operation that unblocks serve(); never call from a signal handler.
  void stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace sphinx
