// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>

#include <cstdint>
#include <nlohmann/json.hpp>
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

}  // namespace

bool valid_product(const Product& product) noexcept {
  return product.id != 0 && product.version != 0 &&
         product.price_cents <= max_product_price_cents && valid_utf8_name(product.name);
}

std::string make_product_cache_key(std::uint64_t id) {
  if (id == 0) {
    throw std::invalid_argument{"product id must be positive"};
  }
  // 使用独立命名空间，避免与其他缓存 key 冲突；格式变更时可更新版本前缀。
  return "product:v2:" + std::to_string(id);
}

std::string encode_product_cache(const Product& product) {
  if (!valid_product(product)) {
    throw std::invalid_argument{"invalid product cache value"};
  }
  return nlohmann::json{{"id", product.id},
                        {"name", product.name},
                        {"price_cents", product.price_cents},
                        {"version", product.version}}
      .dump();
}

std::optional<Product> decode_product_cache(std::string_view bytes) {
  // 缓存内容不可信：限制长度、检查字段，再复用领域对象校验。
  if (bytes.size() > 512) {
    return std::nullopt;
  }
  const auto value = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (!value.is_object() || value.size() != 4 || !value.contains("id") || !value.contains("name") ||
      !value.contains("price_cents") || !value.contains("version") ||
      !value["id"].is_number_unsigned() || !value["name"].is_string() ||
      !value["price_cents"].is_number_unsigned() || !value["version"].is_number_unsigned()) {
    return std::nullopt;
  }
  Product product{value["id"].get<std::uint64_t>(), value["name"].get<std::string>(),
                  value["price_cents"].get<std::uint64_t>(), value["version"].get<std::uint64_t>()};
  if (!valid_product(product)) {
    return std::nullopt;
  }
  return product;
}

}  // namespace sphinx
