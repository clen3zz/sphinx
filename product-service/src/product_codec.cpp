// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>

#include <cstdint>
#include <stdexcept>

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

void append_u64(std::string* out, std::uint64_t value) {
  for (unsigned int byte = 0; byte < 8; ++byte) {
    const auto shift = (7U - byte) * 8U;
    out->push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

std::uint64_t read_u64(std::string_view bytes, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | static_cast<unsigned char>(bytes[offset + i]);
  }
  return value;
}

}  // namespace

bool valid_product(const Product& product) noexcept {
  return product.id != 0 && product.version != 0 &&
         product.price_cents <= max_product_price_cents && valid_utf8_name(product.name);
}

std::string make_product_cache_key(std::uint64_t id) {
  if (id == 0) {
    throw std::invalid_argument{"product id must be positive"};
  }
  return "product:v1:" + std::to_string(id);
}

std::string encode_product_cache(const Product& product) {
  if (!valid_product(product)) {
    throw std::invalid_argument{"invalid product cache value"};
  }
  std::string out;
  out.reserve(30 + product.name.size());
  out.append("SPC1", 4);
  append_u64(&out, product.id);
  append_u64(&out, product.price_cents);
  append_u64(&out, product.version);
  const auto name_size = static_cast<std::uint16_t>(product.name.size());
  out.push_back(static_cast<char>(name_size >> 8U));
  out.push_back(static_cast<char>(name_size & 0xFFU));
  out.append(product.name);
  return out;
}

std::optional<Product> decode_product_cache(std::string_view bytes) {
  constexpr std::size_t header_size = 30;
  if (bytes.size() < header_size || bytes.substr(0, 4) != "SPC1") {
    return std::nullopt;
  }
  const auto name_size = (static_cast<std::size_t>(static_cast<unsigned char>(bytes[28])) << 8U) |
                         static_cast<unsigned char>(bytes[29]);
  if (name_size == 0 || name_size > max_product_name_bytes ||
      bytes.size() != header_size + name_size) {
    return std::nullopt;
  }
  Product product;
  product.id = read_u64(bytes, 4);
  product.price_cents = read_u64(bytes, 12);
  product.version = read_u64(bytes, 20);
  product.name.assign(bytes.data() + header_size, name_size);
  if (!valid_product(product)) {
    return std::nullopt;
  }
  return product;
}

}  // namespace sphinx
