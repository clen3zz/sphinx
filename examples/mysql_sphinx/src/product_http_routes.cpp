// SPDX-License-Identifier: Apache-2.0
#include "product_http_routes.h"

// TODO(agent): Include nlohmann/json.hpp here and implement only route parsing/serialization.
// Exact HTTP contract, error map, and edge cases are fixed in IMPLEMENTATION_PLAN.md. Do not
// create MySQL or cache connections here; current_service() supplies the per-worker instance.

namespace sphinx {

void install_product_routes(httplib::Server& server,
                            std::function<ProductService&()> current_service) {
  // TODO(agent): Register GET and PUT for /products/([^/]+). GET accepts only empty query or
  // fresh=1; PUT requires exact JSON fields name, price_cents, expected_version. Parse full
  // positive uint64 IDs using from_chars, reject overflow, floats, negatives, unknown fields,
  // oversized bodies, invalid UTF-8, and unsupported Content-Type with 400/413/415 as specified.
  // Call service.get(id, fresh) or service.update(request) exactly once. Add Cache-Control:
  // no-store, X-Cache for GET and X-Cache-Invalidation: failed for a committed PUT where erase
  // failed; log the invalidation failure as a sanitized category. Catch StoreError at context
  // creation and bad_alloc/other exceptions at the boundary; never include credentials, SQL,
  // or raw exception text in HTTP responses.
  (void)server;
  (void)current_service;
}

}  // namespace sphinx
