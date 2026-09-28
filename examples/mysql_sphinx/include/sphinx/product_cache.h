// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

namespace sphinx {

/// Raw cache boundary. Values may contain zero bytes. Each instance is confined to one worker
/// thread. All transport/protocol failures throw CacheError; a missing key returns nullopt.
class ProductCache {
 public:
  virtual ~ProductCache() = default;
  virtual std::optional<std::string> get(std::string_view key) = 0;
  /// Requires ttl_seconds in 1..2,592,000; returns only after STORED is acknowledged.
  virtual void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) = 0;
  /// Idempotent: a missing key is success. Throws CacheError on transport/protocol failure.
  virtual void erase(std::string_view key) = 0;
};

}  // namespace sphinx
