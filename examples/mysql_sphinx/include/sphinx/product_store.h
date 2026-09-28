// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

namespace sphinx {

enum class StoreUpdateStatus : std::uint8_t { Updated, NotFound, Conflict };

struct StoreUpdateResult {
  StoreUpdateStatus status = StoreUpdateStatus::NotFound;
  /// Present exactly when status == Updated, with the committed row/version.
  std::optional<Product> product;
};

/// Primary database boundary. One instance belongs to one worker thread; implementations must not
/// share a MYSQL handle across threads. No method silently retries an ambiguous commit.
class ProductStore {
 public:
  virtual ~ProductStore() = default;

  /// Requires id > 0. A missing row is nullopt. Query/connection failure throws StoreError.
  virtual std::optional<Product> find(std::uint64_t id) = 0;

  /// Requires a valid request. Atomically compare version and update one existing row. NotFound and
  /// Conflict are ordinary results; other SQL failures throw StoreError. Never creates a row.
  virtual StoreUpdateResult update(const UpdateProductRequest& request) = 0;
};

}  // namespace sphinx
