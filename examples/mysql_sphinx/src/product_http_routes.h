// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <httplib.h>
#include <sphinx/product_service.h>

#include <functional>

namespace sphinx {

/// Internal boundary. The callback returns the service owned by the current HTTP worker and may
/// throw StoreError during thread setup; each handler maps Unavailable to 503 and other setup
/// failures to a sanitized 500 response.
void install_product_routes(httplib::Server& server,
                            std::function<ProductService&()> current_service);

}  // namespace sphinx
