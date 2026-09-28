// SPDX-License-Identifier: Apache-2.0
#include <mysql.h>
#include <sphinx/mysql_product_store.h>

#include <cassert>
#include <exception>
#include <mutex>
#include <thread>

// TODO(agent): This optional target is wired to libmysqlclient. Use mysql.h here only; never
// include it in the public header. Follow the exact prepared-statement and transaction sequence
// in IMPLEMENTATION_PLAN.md. Never put credentials or SQL input into StoreError::what().

namespace sphinx {
namespace {

std::mutex runtime_mutex;
bool library_initialized = false;
std::size_t active_thread_guards = 0;
thread_local bool current_thread_has_guard = false;

bool has_embedded_nul(const std::string& value) noexcept {
  return value.find('\0') != std::string::npos;
}

void validate_options(const MySqlOptions& options) {
  if (options.host.empty() || options.user.empty() || options.database.empty() ||
      options.port == 0 || options.connect_timeout_seconds == 0 ||
      options.read_timeout_seconds == 0 || options.write_timeout_seconds == 0 ||
      has_embedded_nul(options.host) || has_embedded_nul(options.user) ||
      has_embedded_nul(options.password) || has_embedded_nul(options.database)) {
    throw std::invalid_argument{"invalid MySQL connection options"};
  }
}

}  // namespace

struct MySqlProductStore::Impl {
  explicit Impl(const MySqlOptions& source_options)
      : options{source_options}, owner_thread{std::this_thread::get_id()} {}

  ~Impl() {
    assert(owner_thread == std::this_thread::get_id());
    reset_connection();
  }

  void check_owner() const {
    if (owner_thread != std::this_thread::get_id()) {
      throw StoreError{StoreErrorCode::Unexpected, "MySQL store used from a non-owner thread"};
    }
  }

  void reset_connection() noexcept {
    if (find_statement != nullptr) {
      mysql_stmt_close(find_statement);
      find_statement = nullptr;
    }
    if (lock_statement != nullptr) {
      mysql_stmt_close(lock_statement);
      lock_statement = nullptr;
    }
    if (update_statement != nullptr) {
      mysql_stmt_close(update_statement);
      update_statement = nullptr;
    }
    if (connection != nullptr) {
      mysql_close(connection);
      connection = nullptr;
    }
  }

  MySqlOptions options;
  const std::thread::id owner_thread;
  MYSQL* connection = nullptr;
  MYSQL_STMT* find_statement = nullptr;
  MYSQL_STMT* lock_statement = nullptr;
  MYSQL_STMT* update_statement = nullptr;
};

MySqlRuntime::MySqlRuntime() {
  std::lock_guard<std::mutex> lock{runtime_mutex};
  if (library_initialized) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL runtime already exists"};
  }
  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL client library initialization failed"};
  }
  library_initialized = true;
}

MySqlRuntime::~MySqlRuntime() {
  std::lock_guard<std::mutex> lock{runtime_mutex};
  if (active_thread_guards != 0) {
    std::terminate();
  }
  if (library_initialized) {
    mysql_library_end();
    library_initialized = false;
  }
}

MySqlThreadGuard::MySqlThreadGuard() {
  std::lock_guard<std::mutex> lock{runtime_mutex};
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
  std::lock_guard<std::mutex> lock{runtime_mutex};
  mysql_thread_end();
  current_thread_has_guard = false;
  assert(active_thread_guards > 0);
  --active_thread_guards;
}

MySqlProductStore::MySqlProductStore(const MySqlOptions& options) : _impl{nullptr} {
  validate_options(options);
  if (!current_thread_has_guard) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL store requires a thread guard"};
  }
  _impl = std::make_unique<Impl>(options);
}

MySqlProductStore::~MySqlProductStore() = default;

std::optional<Product> MySqlProductStore::find(std::uint64_t id) {
  // TODO(agent): Require id > 0. Execute SELECT id,name,price_cents,version FROM products WHERE
  // id=? using a prepared statement on the primary connection. Nullopt only on zero rows.
  // Validate all numeric/UTF-8 domain fields and exactly one row; invalid row -> InvalidData.
  // On connection/SQL error throw Unavailable, discard broken handle, and reconnect only on next
  // independent call. Never use a read replica for this operation.
  (void)id;
  throw StoreError{StoreErrorCode::Unexpected, "MySQL find implementation pending"};
}

StoreUpdateResult MySqlProductStore::update(const UpdateProductRequest& request) {
  // TODO(agent): Run the exact BEGIN -> SELECT ... FOR UPDATE -> version comparison -> UPDATE
  // with version predicate -> COMMIT algorithm in IMPLEMENTATION_PLAN.md. Roll back on all known
  // precommit failures. On missing row return NotFound; on version mismatch return Conflict.
  // If COMMIT response is lost/failed, discard connection and throw CommitUnknown, with no retry.
  // Return Updated only after an acknowledged COMMIT with the new version. Never auto-insert.
  (void)request;
  throw StoreError{StoreErrorCode::Unexpected, "MySQL update implementation pending"};
}

}  // namespace sphinx
