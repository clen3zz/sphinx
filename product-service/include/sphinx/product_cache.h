// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

namespace sphinx {

/// 商品缓存 key 的共同字节限制：非空、不超过 250 字节、不含空白或控制字符。
bool valid_product_cache_key(std::string_view key) noexcept;

/// 缓存接口：值可以包含零字节，每个实例只在所属工作线程使用。
/// 传输或协议失败抛出 CacheError；key 不存在返回 nullopt。
class ProductCache {
 public:
  virtual ~ProductCache() = default;
  virtual std::optional<std::string> get(std::string_view key) = 0;
  /// ttl_seconds 必须在 1..2,592,000；收到 STORED 确认后才返回。
  virtual void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) = 0;
  /// 幂等删除：key 不存在也算成功；传输或协议失败时抛出 CacheError。
  virtual void erase(std::string_view key) = 0;
};

}  // namespace sphinx
