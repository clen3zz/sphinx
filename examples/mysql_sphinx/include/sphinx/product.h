// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace sphinx {

/// The database row and cache value. id/version are positive; name is valid UTF-8, 1..128 bytes;
/// price_cents is at most max_product_price_cents. Values crossing an interface must obey this.
struct Product {
  std::uint64_t id = 0;
  std::string name;
  std::uint64_t price_cents = 0;
  std::uint64_t version = 0;
};

inline constexpr std::uint64_t max_product_price_cents = 1'000'000'000'000ULL;
inline constexpr std::size_t max_product_name_bytes = 128;

/// Validates the complete domain object, including UTF-8 and absence of control characters.
bool valid_product(const Product& product) noexcept;

struct UpdateProductRequest {
  std::uint64_t id = 0;
  std::string name;
  std::uint64_t price_cents = 0;
  std::uint64_t expected_version = 0;
};

/// A successful update returns version = expected_version + 1.
enum class ProductStatus : std::uint8_t {
  Ok,
  InvalidArgument,
  NotFound,
  Conflict,
  StoreUnavailable,
  CommitUnknown,
  InternalError,
};

/// Hit means a decoded cache value. Miss means no key; Bypass means cache I/O failed or the caller
/// requested a primary read; Corrupt means a key existed but failed validation.
enum class CacheSource : std::uint8_t { NotChecked, Hit, Miss, Bypass, Corrupt };

struct GetProductResult {
  ProductStatus status = ProductStatus::InternalError;
  std::optional<Product> product;
  CacheSource cache_source = CacheSource::NotChecked;
};

struct UpdateProductResult {
  ProductStatus status = ProductStatus::InternalError;
  std::optional<Product> product;
  /// True only when the DB committed but cache deletion failed; success still means DB success.
  bool cache_invalidation_failed = false;
};

enum class StoreErrorCode : std::uint8_t { Unavailable, InvalidData, CommitUnknown, Unexpected };

/// Store implementations throw this for failures; they never turn SQL failure into NotFound.
class StoreError final : public std::runtime_error {
 public:
  StoreError(StoreErrorCode code, const std::string& message)
      : std::runtime_error{message}, _code{code} {}
  StoreErrorCode code() const noexcept { return _code; }

 private:
  StoreErrorCode _code;
};

class CacheError final : public std::runtime_error {
 public:
  explicit CacheError(const std::string& message) : std::runtime_error{message} {}
};

}  // namespace sphinx
