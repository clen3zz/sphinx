// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product/application/ports/product_store.h>
#include <sphinx/product/backends/mysql/mysql_options.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sphinx {

/// 每个工作线程独占一个连接。数据 SQL 使用预处理语句，事务结束使用
/// mysql_commit/mysql_rollback；查询直接访问主 MySQL 连接。
/// 传输失败后丢弃连接，下一个独立操作才重连，不在当前事务中自动重试。
class MySqlProductStore final : public ProductStore {
 public:
  explicit MySqlProductStore(const MySqlOptions& options);
  ~MySqlProductStore() override;
  MySqlProductStore(const MySqlProductStore&) = delete;
  MySqlProductStore& operator=(const MySqlProductStore&) = delete;

  std::optional<Product> find(std::uint64_t id) override;
  std::vector<std::optional<Product>> find_many(const std::vector<std::uint64_t>& ids) override;
  StoreUpdateResult update(const UpdateProductRequest& request) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace sphinx
