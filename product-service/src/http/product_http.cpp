// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/http/product_http.h>

#include <mutex>
#include <stdexcept>
#include <utility>

#include "product_http_routes.h"

namespace sphinx {
namespace {

ProductHttpConfig checked_config(ProductHttpConfig config) {
  if (config.bind_address.empty() || config.port == 0 || config.worker_count == 0 ||
      config.worker_count > 64) {
    throw std::invalid_argument{"invalid product HTTP configuration"};
  }
  return config;
}

}  // namespace

struct ProductHttpServer::Impl {
  Impl(ProductHttpConfig source_config, ProductServiceFactory factory,
       const ProductSharedState& shared_state)
      : config{std::move(source_config)},
        current_service{std::move(factory)},
        shared{shared_state} {
    if (!current_service) {
      throw std::invalid_argument{"product service factory is empty"};
    }
  }

  ProductHttpConfig config;
  ProductServiceFactory current_service;
  const ProductSharedState& shared;
  httplib::Server server;
  std::mutex state_mutex;
  bool stopping = false;
  bool serve_called = false;
};

ProductHttpServer::ProductHttpServer(ProductHttpConfig config,
                                     ProductServiceFactory current_service,
                                     const ProductSharedState& shared)
    : _impl{std::make_unique<Impl>(checked_config(std::move(config)), std::move(current_service),
                                   shared)} {
  _impl->server.set_payload_max_length(65536);
  _impl->server.set_read_timeout(2, 0);
  _impl->server.set_write_timeout(2, 0);
  const auto worker_count = _impl->config.worker_count;
  // 线程池固定大小；同步 MySQL/缓存 I/O 占用 HTTP Worker，不阻塞 sphinxd 的 Reactor。
  _impl->server.new_task_queue = [worker_count] {
    return new httplib::ThreadPool{worker_count, worker_count, 256};
  };
}

ProductHttpServer::~ProductHttpServer() = default;

bool ProductHttpServer::serve() {
  {
    std::lock_guard lock{_impl->state_mutex};
    if (_impl->serve_called) {
      throw std::logic_error{"ProductHttpServer::serve may only be called once"};
    }
    _impl->serve_called = true;
    if (_impl->stopping) {
      return true;
    }
  }

  install_product_routes(_impl->server, _impl->current_service, _impl->shared,
                         _impl->config.cache_backend, _impl->config.cache_policy);

  {
    std::lock_guard lock{_impl->state_mutex};
    if (_impl->stopping) {
      return true;
    }
    if (!_impl->server.bind_to_port(_impl->config.bind_address, _impl->config.port)) {
      return false;
    }
  }

  const bool listen_result = _impl->server.listen_after_bind();
  std::lock_guard lock{_impl->state_mutex};
  return _impl->stopping || listen_result;
}

void ProductHttpServer::stop() noexcept {
  std::lock_guard lock{_impl->state_mutex};
  _impl->stopping = true;
  _impl->server.stop();
}

}  // namespace sphinx
