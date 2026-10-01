// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace sphinx {

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

/// 当前线程是否持有 MySQL 线程环境，供存储适配器检查生命周期。
bool mysql_thread_initialized() noexcept;

}  // namespace sphinx
