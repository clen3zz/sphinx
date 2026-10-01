// SPDX-License-Identifier: Apache-2.0
#include <mysql.h>
#include <sphinx/product/application/ports/product_store.h>
#include <sphinx/product/backends/mysql/mysql_runtime.h>

#include <cassert>
#include <cstddef>
#include <exception>
#include <mutex>

namespace sphinx {

namespace {

std::mutex runtime_mutex;
bool library_initialized = false;
std::size_t active_thread_guards = 0;
thread_local bool current_thread_has_guard = false;

}  // namespace

bool mysql_thread_initialized() noexcept { return current_thread_has_guard; }

MySqlRuntime::MySqlRuntime() {
  // 客户端库按进程初始化一次，必须早于所有 HTTP 工作线程。
  std::lock_guard lock{runtime_mutex};
  if (library_initialized) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL runtime already exists"};
  }
  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL client library initialization failed"};
  }
  library_initialized = true;
}

MySqlRuntime::~MySqlRuntime() {
  std::lock_guard lock{runtime_mutex};
  if (active_thread_guards != 0) {
    std::terminate();
  }
  if (library_initialized) {
    mysql_library_end();
    library_initialized = false;
  }
}

MySqlThreadGuard::MySqlThreadGuard() {
  // 每个工作线程单独完成 MySQL 线程初始化，且只允许在该线程使用自己的 store。
  std::lock_guard lock{runtime_mutex};
  if (!library_initialized || current_thread_has_guard) {
    throw StoreError{StoreErrorCode::Unexpected, "invalid MySQL thread initialization order"};
  }
  if (mysql_thread_init() != 0) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL thread initialization failed"};
  }
  current_thread_has_guard = true;
  ++active_thread_guards;
}

MySqlThreadGuard::~MySqlThreadGuard() {
  assert(current_thread_has_guard);
  std::lock_guard lock{runtime_mutex};
  mysql_thread_end();
  current_thread_has_guard = false;
  assert(active_thread_guards > 0);
  --active_thread_guards;
}

}  // namespace sphinx
