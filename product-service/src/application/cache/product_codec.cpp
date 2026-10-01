// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product/application/cache/product_cache_policy.h>
#include <sphinx/product/application/cache/product_codec.h>

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace sphinx {
namespace {

std::optional<Product> decode_product(const nlohmann::json& value) {
  if (!value.is_object() || value.size() != 4 || !value.contains("id") || !value.contains("name") ||
      !value.contains("price_cents") || !value.contains("version") ||
      !value["id"].is_number_unsigned() || !value["name"].is_string() ||
      !value["price_cents"].is_number_unsigned() || !value["version"].is_number_unsigned()) {
    return std::nullopt;
  }
  Product product{value["id"].get<std::uint64_t>(), value["name"].get<std::string>(),
                  value["price_cents"].get<std::uint64_t>(), value["version"].get<std::uint64_t>()};
  return valid_product(product) ? std::optional<Product>{std::move(product)} : std::nullopt;
}

}  // namespace

std::string make_product_cache_key(std::uint64_t id) {
  if (id == 0) {
    throw std::invalid_argument{"product id must be positive"};
  }
  // 使用独立命名空间，避免与其他缓存 key 冲突；格式变更时可更新版本前缀。
  return "product:v3:" + std::to_string(id);
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
  return decode_product(nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false));
}

std::string encode_product_not_found(std::uint64_t id) {
  if (id == 0) {
    throw std::invalid_argument{"product id must be positive"};
  }
  return nlohmann::json{{"id", id}, {"not_found", true}}.dump();
}

DecodedProductCacheEntry decode_product_cache_entry(std::string_view payload,
                                                    std::uint64_t expected_id) {
  if (expected_id == 0 || payload.size() > 512) {
    return {};
  }

  const auto value = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
  if (!value.is_object()) {
    return {};
  }

  auto product = decode_product(value);
  if (product && product->id == expected_id) {
    return {CacheEntryKind::Product, std::move(product)};
  }

  if (value.size() == 2 && value.contains("id") && value.contains("not_found") &&
      value["id"].is_number_unsigned() && value["not_found"].is_boolean() &&
      value["id"].get<std::uint64_t>() == expected_id && value["not_found"].get<bool>()) {
    return {CacheEntryKind::NotFound, std::nullopt};
  }

  return {};
}

std::uint32_t product_cache_ttl(std::uint64_t id, const ProductCachePolicy& policy) noexcept {
  if (policy.mode != CachePolicyMode::Protected) {
    return policy.ttl_seconds;
  }

  constexpr std::uint64_t hash_increment = 0x9E3779B97F4A7C15ULL;
  constexpr std::uint64_t hash_multiplier_one = 0xBF58476D1CE4E5B9ULL;
  constexpr std::uint64_t hash_multiplier_two = 0x94D049BB133111EBULL;
  std::uint64_t hash = id + hash_increment;
  hash = (hash ^ (hash >> 30U)) * hash_multiplier_one;
  hash = (hash ^ (hash >> 27U)) * hash_multiplier_two;
  hash ^= hash >> 31U;

  const auto jitter = static_cast<std::uint64_t>(policy.ttl_jitter_seconds);
  const auto span = jitter * 2U + 1U;
  const auto offset =
      static_cast<std::int64_t>(hash % span) - static_cast<std::int64_t>(policy.ttl_jitter_seconds);
  const auto ttl = static_cast<std::int64_t>(policy.ttl_seconds) + offset;
  if (ttl < 1) {
    return 1;
  }
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(ttl), max_product_cache_ttl_seconds));
}

}  // namespace sphinx
