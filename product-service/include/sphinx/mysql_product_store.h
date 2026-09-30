// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_store.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sphinx {

struct MySqlOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user;
  std::string password;
  std::string database;
  std::uint32_t connect_timeout_seconds = 2;
  std::uint32_t read_timeout_seconds = 2;
  std::uint32_t write_timeout_seconds = 2;
};

/// 进程级 MySQL 客户端运行时：在 HTTP 工作线程启动前创建，在线程退出后销毁。
class MySqlRuntime final {
 public:
  MySqlRuntime();
  ~MySqlRuntime();
  MySqlRuntime(const MySqlRuntime&) = delete;
  MySqlRuntime& operator=(const MySqlRuntime&) = delete;
};

/// 每个工作线程各有一个；在本线程创建 MySqlProductStore 前构造，并在其销毁后析构。
class MySqlThreadGuard final {
 public:
  MySqlThreadGuard();
  ~MySqlThreadGuard();
  MySqlThreadGuard(const MySqlThreadGuard&) = delete;
  MySqlThreadGuard& operator=(const MySqlThreadGuard&) = delete;
};

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
