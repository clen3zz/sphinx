// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_http.h>

#include <exception>
#include <iostream>
#include <utility>

int main() {
  // TODO(agent): Parse the exact environment contract in IMPLEMENTATION_PLAN.md without printing
  // credentials. Validate all required values and numeric ranges before creating the server.
  // Block SIGINT/SIGTERM before worker creation; a control thread uses sigwait and calls stop().
  // Keep MySqlRuntime lifetime inside ProductHttpServer. On bind failure return 1; on normal
  // signal-driven shutdown return 0. Catch exceptions here, log only sanitized category, and
  // return 1. Never retry an ambiguous commit in this layer.
  try {
    sphinx::ProductHttpConfig config;
    sphinx::ProductHttpServer server{std::move(config)};
    return server.serve() ? 0 : 1;
  } catch (const std::exception&) {
    std::cerr << "product service startup failed\n";
    return 1;
  }
}
