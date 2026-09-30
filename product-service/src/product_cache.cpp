// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_cache.h>

namespace sphinx {

bool valid_product_cache_key(std::string_view key) noexcept {
  if (key.empty() || key.size() > 250) {
    return false;
  }
  for (const char byte : key) {
    const auto value = static_cast<unsigned char>(byte);
    if (value <= 0x20U || value == 0x7FU) {
      return false;
    }
  }
  return true;
}

}  // namespace sphinx
