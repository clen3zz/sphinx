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

ProductHttpServer::ProductHttpServer(ProductHttpConfig config,
                                     ProductServiceFactory current_service,
                                     const ProductSharedState& shared)
    : _config{checked_config(std::move(config))},
      _current_service{std::move(current_service)},
      _shared{shared} {
  if (!_current_service) {
    throw std::invalid_argument{"product service factory is empty"};
  }
  _server.set_payload_max_length(65536);
  _server.set_read_timeout(2, 0);
  _server.set_write_timeout(2, 0);
  const auto worker_count = _config.worker_count;
  // 线程池固定大小；同步 MySQL/缓存 I/O 占用 HTTP Worker，不阻塞 sphinxd 的 Reactor。
  _server.new_task_queue = [worker_count] {
    return new httplib::ThreadPool{worker_count, worker_count, 256};
  };
}

ProductHttpServer::~ProductHttpServer() = default;

bool ProductHttpServer::serve() {
  {
    std::lock_guard lock{_state_mutex};
    if (_serve_called) {
      throw std::logic_error{"ProductHttpServer::serve may only be called once"};
    }
    _serve_called = true;
    if (_stopping) {
      return true;
    }
  }

  install_product_routes(_server, _current_service, _shared, _config.cache_backend,
                         _config.cache_policy);

  {
    std::lock_guard lock{_state_mutex};
    if (_stopping) {
      return true;
    }
    if (!_server.bind_to_port(_config.bind_address, _config.port)) {
      return false;
    }
  }

  const bool listen_result = _server.listen_after_bind();
  std::lock_guard lock{_state_mutex};
  return _stopping || listen_result;
}

void ProductHttpServer::stop() noexcept {
  std::lock_guard lock{_state_mutex};
  _stopping = true;
  _server.stop();
}

}  // namespace sphinx
