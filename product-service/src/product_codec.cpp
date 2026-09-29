// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_codec.h>

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace sphinx {

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
