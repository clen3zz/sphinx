// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_cache.h>

#include <stdexcept>
#include <utility>

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

void validate_product_cache_keys(const std::vector<std::string>& keys) {
  if (keys.size() > max_product_batch_size) {
    throw std::invalid_argument{"product cache batch exceeds 32 keys"};
  }
  for (const auto& key : keys) {
    if (!valid_product_cache_key(key)) {
      throw std::invalid_argument{"product cache batch contains an invalid key"};
    }
  }
}

std::vector<std::optional<std::string>> ProductCache::get_many(
    const std::vector<std::string>& keys) {
  validate_product_cache_keys(keys);
  std::vector<std::optional<std::string>> values;
  values.reserve(keys.size());
  for (const auto& key : keys) {
    values.push_back(get(key));
  }
  return values;
}

void validate_product_cache_writes(const std::vector<CacheWriteEntry>& entries) {
  if (entries.size() > max_product_batch_size) {
    throw std::invalid_argument{"product cache batch exceeds 32 writes"};
  }
  for (const auto& entry : entries) {
    if (!valid_product_cache_key(entry.key)) {
      throw std::invalid_argument{"product cache batch contains an invalid key"};
    }
    if (!valid_product_cache_ttl(entry.ttl_seconds)) {
      throw std::invalid_argument{"product cache batch contains an invalid TTL"};
    }
  }
}

void ProductCache::put_many(const std::vector<CacheWriteEntry>& entries) {
  validate_product_cache_writes(entries);
  for (const auto& entry : entries) {
    put(entry.key, entry.value, entry.ttl_seconds);
  }
}

}  // namespace sphinx
