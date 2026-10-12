// SPDX-License-Identifier: Apache-2.0
#include <bootstrap/product_config.h>
#include <bootstrap/product_worker.h>
#include <http/product_http.h>

#include <mutex>
#include <stdexcept>

#include "product_http_routes.h"

namespace sphinx {
namespace {

void validate_http_config(const ProductHttpConfig& config) {
  if (config.bind_address.empty() || config.port == 0 || config.worker_count == 0 ||
      config.worker_count > 64) {
    throw std::invalid_argument{"invalid product HTTP configuration"};
  }
}

}  // namespace

ProductHttpServer::ProductHttpServer(const ProductRuntimeConfig& config, ProductSharedState& shared)
    : _config{config}, _shared{shared} {
  validate_http_config(_config.http);
  _server.set_payload_max_length(65536);
  _server.set_read_timeout(2, 0);
  _server.set_write_timeout(2, 0);
  _server.new_task_queue = [this] { return make_product_worker_queue(_config, _shared, _server); };
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

  install_product_routes(_server, _shared, _config);

  {
    std::lock_guard lock{_state_mutex};
    if (_stopping) {
      return true;
    }
    if (!_server.bind_to_port(_config.http.bind_address, _config.http.port)) {
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
