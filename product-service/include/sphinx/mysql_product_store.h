// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product_store.h>

#include <memory>
#include <string>

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

/// Exactly one process owner, created before HTTP workers and destroyed after they join.
class MySqlRuntime final {
 public:
  MySqlRuntime();
  ~MySqlRuntime();
  MySqlRuntime(const MySqlRuntime&) = delete;
  MySqlRuntime& operator=(const MySqlRuntime&) = delete;
};

/// One per worker thread; construct before MySqlProductStore, destroy after it on that thread.
class MySqlThreadGuard final {
 public:
  MySqlThreadGuard();
  ~MySqlThreadGuard();
  MySqlThreadGuard(const MySqlThreadGuard&) = delete;
  MySqlThreadGuard& operator=(const MySqlThreadGuard&) = delete;
};

/// One connection per worker. Data SQL uses prepared statements; fixed transaction control uses
/// mysql_commit/mysql_rollback. Reads go to the primary MySQL connection.
/// On transport error, discard the handle and reconnect on the next independent operation.
class MySqlProductStore final : public ProductStore {
 public:
  explicit MySqlProductStore(const MySqlOptions& options);
  ~MySqlProductStore() override;
  MySqlProductStore(const MySqlProductStore&) = delete;
  MySqlProductStore& operator=(const MySqlProductStore&) = delete;

  std::optional<Product> find(std::uint64_t id) override;
  StoreUpdateResult update(const UpdateProductRequest& request) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace sphinx
