// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/application/product_limits.h>
#include <sphinx/product/domain/product.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sphinx {

enum class StoreErrorCode : std::uint8_t { Unavailable, InvalidData, CommitUnknown, Unexpected };

/// 存储层出错时抛出此异常，不能把 SQL 失败伪装成 NotFound。
class StoreError final : public std::runtime_error {
 public:
  StoreError(StoreErrorCode code, const std::string& message)
      : std::runtime_error{message}, _code{code} {}
  StoreErrorCode code() const noexcept { return _code; }

 private:
  StoreErrorCode _code;
};

enum class StoreUpdateStatus : std::uint8_t { Updated, NotFound, Conflict };

struct StoreUpdateResult {
  StoreUpdateStatus status = StoreUpdateStatus::NotFound;
  /// 仅当 status == Updated 时有值，包含已提交的商品及新版本号。
  std::optional<Product> product;
};

/// 权威存储接口：一个实例只属于一个工作线程，不能跨线程共享存储连接。
/// 提交结果不确定时，任何方法都不能悄悄重试。
class ProductStore {
 public:
  virtual ~ProductStore() = default;

  /// 要求 id > 0；记录不存在返回 nullopt；查询或连接失败抛出 StoreError。
  virtual std::optional<Product> find(std::uint64_t id) = 0;

  /// 保持输入顺序和重复 ID；默认实现逐项查询，批量最多 32 项。
  virtual std::vector<std::optional<Product>> find_many(const std::vector<std::uint64_t>& ids);

  /// 原子地检查版本并更新已有记录，不负责插入新记录。
  /// NotFound 和 Conflict 是正常业务结果；其他 SQL 失败抛出 StoreError。
  virtual StoreUpdateResult update(const UpdateProductRequest& request) = 0;
};

}  // namespace sphinx
