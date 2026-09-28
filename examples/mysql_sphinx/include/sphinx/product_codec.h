// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

namespace sphinx {

/// Namespaced key; v1 changes whenever the binary value format changes. Requires id > 0.
std::string make_product_cache_key(std::uint64_t id);

/// Wire format: ASCII "SPC1", then id/price_cents/version as three big-endian u64s, then a
/// big-endian u16 byte length and exact UTF-8 name bytes. No padding, checksum or trailing bytes.
/// Throws std::invalid_argument if the product violates the domain invariant.
std::string encode_product_cache(const Product& product);

/// Never throws for malformed bytes. Returns nullopt for wrong magic, truncation, trailing bytes,
/// invalid UTF-8/name, zero id/version, or out-of-range price.
std::optional<Product> decode_product_cache(std::string_view bytes);

}  // namespace sphinx
