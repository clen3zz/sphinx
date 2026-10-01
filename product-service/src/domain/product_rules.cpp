// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/domain/product.h>

#include <cstdint>
#include <limits>
#include <string_view>

namespace sphinx {
namespace {

bool valid_utf8_name(std::string_view bytes) noexcept {
  if (bytes.empty() || bytes.size() > max_product_name_bytes) {
    return false;
  }
  for (std::size_t i = 0; i < bytes.size();) {
    const auto first = static_cast<unsigned char>(bytes[i]);
    std::uint32_t code_point = 0;
    std::size_t width = 0;
    if (first < 0x80) {
      code_point = first;
      width = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
      code_point = static_cast<std::uint32_t>(first) & 0x1FU;
      width = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
      code_point = static_cast<std::uint32_t>(first) & 0x0FU;
      width = 3;
    } else if (first >= 0xF0 && first <= 0xF4) {
      code_point = static_cast<std::uint32_t>(first) & 0x07U;
      width = 4;
    } else {
      return false;
    }
    if (width > bytes.size() - i) {
      return false;
    }
    for (std::size_t j = 1; j < width; ++j) {
      const auto next = static_cast<unsigned char>(bytes[i + j]);
      if ((static_cast<std::uint32_t>(next) & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (static_cast<std::uint32_t>(next) & 0x3FU);
    }
    if ((width == 2 && code_point < 0x80) || (width == 3 && code_point < 0x800) ||
        (width == 4 && code_point < 0x10000) || (code_point >= 0xD800 && code_point <= 0xDFFF) ||
        code_point > 0x10FFFF || code_point < 0x20 || (code_point >= 0x7F && code_point <= 0x9F)) {
      return false;
    }
    i += width;
  }
  return true;
}

bool valid_product_fields(std::uint64_t id, std::string_view name, std::uint64_t price_cents,
                          std::uint64_t version) noexcept {
  return id != 0 && version != 0 && price_cents <= max_product_price_cents && valid_utf8_name(name);
}

}  // namespace

bool valid_product(const Product& product) noexcept {
  return valid_product_fields(product.id, product.name, product.price_cents, product.version);
}

bool valid_update_request(const UpdateProductRequest& request) noexcept {
  return request.expected_version != 0 &&
         request.expected_version != std::numeric_limits<std::uint64_t>::max() &&
         valid_product_fields(request.id, request.name, request.price_cents, 1);
}

}  // namespace sphinx
