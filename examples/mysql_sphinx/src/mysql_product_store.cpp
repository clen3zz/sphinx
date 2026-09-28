// SPDX-License-Identifier: Apache-2.0
#include <mysql.h>
#include <errmsg.h>
#include <mysqld_error.h>
#include <sphinx/mysql_product_store.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
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

bool is_unavailable_error(unsigned int error) noexcept {
  switch (error) {
    case CR_CONNECTION_ERROR:
    case CR_CONN_HOST_ERROR:
    case CR_IPSOCK_ERROR:
    case CR_UNKNOWN_HOST:
    case CR_SERVER_GONE_ERROR:
    case CR_TCP_CONNECTION:
    case CR_SERVER_LOST:
    case CR_SSL_CONNECTION_ERROR:
    case CR_SERVER_LOST_EXTENDED:
    case ER_SERVER_SHUTDOWN:
    case ER_TOO_MANY_USER_CONNECTIONS:
    case ER_LOCK_WAIT_TIMEOUT:
    case ER_LOCK_DEADLOCK:
    case ER_QUERY_TIMEOUT:
      return true;
    default:
      return false;
  }
}

StoreErrorCode classify_mysql_error(unsigned int error) noexcept {
  return is_unavailable_error(error) ? StoreErrorCode::Unavailable : StoreErrorCode::Unexpected;
}

[[noreturn]] void throw_mysql_error(unsigned int error, const char* message) {
  throw StoreError{classify_mysql_error(error), message};
}

void bind_unsigned_result(MYSQL_BIND* binding, std::uint64_t* value, bool* is_null, bool* error) {
  binding->buffer_type = MYSQL_TYPE_LONGLONG;
  binding->buffer = value;
  binding->is_unsigned = true;
  binding->is_null = is_null;
  binding->error = error;
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
    if (owner_thread != std::this_thread::get_id() || !current_thread_has_guard) {
      throw StoreError{StoreErrorCode::Unexpected, "MySQL store used from a non-owner thread"};
    }
  }

  void ensure_connection() {
    if (connection != nullptr) {
      return;
    }

    MYSQL* handle = mysql_init(nullptr);
    if (handle == nullptr) {
      throw StoreError{StoreErrorCode::Unexpected, "MySQL connection allocation failed"};
    }

    const unsigned int connect_timeout = options.connect_timeout_seconds;
    const unsigned int read_timeout = options.read_timeout_seconds;
    const unsigned int write_timeout = options.write_timeout_seconds;
    const unsigned int protocol = MYSQL_PROTOCOL_TCP;
    if (mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout) != 0 ||
        mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &read_timeout) != 0 ||
        mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout) != 0 ||
        mysql_options(handle, MYSQL_OPT_PROTOCOL, &protocol) != 0) {
      mysql_close(handle);
      throw StoreError{StoreErrorCode::Unexpected, "MySQL connection option setup failed"};
    }

    // MySQL 8.4 disables automatic reconnect by default, including after mysql_real_connect();
    // MYSQL_OPT_RECONNECT is deprecated and emits a warning even when set to false.
    if (mysql_real_connect(handle, options.host.c_str(), options.user.c_str(),
                           options.password.c_str(), options.database.c_str(), options.port,
                           nullptr, 0) == nullptr) {
      mysql_close(handle);
      throw StoreError{StoreErrorCode::Unavailable, "MySQL connection unavailable"};
    }

    connection = handle;
    if (mysql_set_character_set(connection, "utf8mb4") != 0) {
      reset_connection();
      throw StoreError{StoreErrorCode::Unexpected, "MySQL character set setup failed"};
    }
  }

  MYSQL_STMT* prepare_find_statement() {
    if (find_statement != nullptr) {
      return find_statement;
    }

    constexpr char query[] =
        "SELECT id, name, price_cents, version FROM products WHERE id = ?";
    MYSQL_STMT* statement = mysql_stmt_init(connection);
    if (statement == nullptr) {
      throw StoreError{StoreErrorCode::Unexpected, "MySQL statement allocation failed"};
    }
    if (mysql_stmt_prepare(statement, query, sizeof(query) - 1) != 0) {
      const auto error = mysql_stmt_errno(statement);
      mysql_stmt_close(statement);
      if (is_unavailable_error(error)) {
        reset_connection();
      }
      throw_mysql_error(error, "MySQL product query preparation failed");
    }
    if (mysql_stmt_param_count(statement) != 1 || mysql_stmt_field_count(statement) != 4) {
      mysql_stmt_close(statement);
      throw StoreError{StoreErrorCode::Unexpected, "MySQL product query shape is invalid"};
    }
    find_statement = statement;
    return statement;
  }

  /// Clear buffered rows and reset the statement for reuse. On failure, close the whole
  /// connection so the next independent operation starts from a known state.
  unsigned int clear_find_result(MYSQL_STMT* statement) noexcept {
    if (find_statement != statement || connection == nullptr) {
      return 0;
    }
    const bool free_failed = mysql_stmt_free_result(statement);
    const auto free_error = free_failed ? mysql_stmt_errno(statement) : 0;
    const bool reset_failed = mysql_stmt_reset(statement);
    const auto reset_error = reset_failed ? mysql_stmt_errno(statement) : 0;
    if (!free_failed && !reset_failed) {
      return 0;
    }
    reset_connection();
    return reset_error != 0 ? reset_error : (free_error != 0 ? free_error : 1);
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
  _impl->check_owner();
  if (id == 0) {
    throw std::invalid_argument{"product id must be positive"};
  }

  _impl->ensure_connection();
  MYSQL_STMT* statement = _impl->prepare_find_statement();

  std::uint64_t requested_id = id;
  MYSQL_BIND parameter{};
  parameter.buffer_type = MYSQL_TYPE_LONGLONG;
  parameter.buffer = &requested_id;
  parameter.is_unsigned = true;
  if (mysql_stmt_bind_param(statement, &parameter) != 0) {
    const auto error = mysql_stmt_errno(statement);
    if (is_unavailable_error(error)) {
      _impl->reset_connection();
    }
    throw_mysql_error(error, "MySQL product query parameter binding failed");
  }
  if (mysql_stmt_execute(statement) != 0) {
    const auto error = mysql_stmt_errno(statement);
    if (is_unavailable_error(error)) {
      _impl->reset_connection();
    }
    throw_mysql_error(error, "MySQL product query failed");
  }
  if (mysql_stmt_field_count(statement) != 4) {
    (void)_impl->clear_find_result(statement);
    throw StoreError{StoreErrorCode::Unexpected, "MySQL product query returned an invalid shape"};
  }

  Product product{};
  std::array<char, max_product_name_bytes + 1> name_buffer{};
  unsigned long name_length = 0;
  bool id_is_null = false;
  bool name_is_null = false;
  bool price_is_null = false;
  bool version_is_null = false;
  bool id_error = false;
  bool name_error = false;
  bool price_error = false;
  bool version_error = false;
  MYSQL_BIND results[4]{};
  bind_unsigned_result(&results[0], &product.id, &id_is_null, &id_error);
  results[1].buffer_type = MYSQL_TYPE_STRING;
  results[1].buffer = name_buffer.data();
  results[1].buffer_length = static_cast<unsigned long>(name_buffer.size());
  results[1].length = &name_length;
  results[1].is_null = &name_is_null;
  results[1].error = &name_error;
  bind_unsigned_result(&results[2], &product.price_cents, &price_is_null, &price_error);
  bind_unsigned_result(&results[3], &product.version, &version_is_null, &version_error);

  if (mysql_stmt_bind_result(statement, results) != 0) {
    const auto error = mysql_stmt_errno(statement);
    (void)_impl->clear_find_result(statement);
    if (is_unavailable_error(error)) {
      _impl->reset_connection();
    }
    throw_mysql_error(error, "MySQL product query result binding failed");
  }
  if (mysql_stmt_store_result(statement) != 0) {
    const auto error = mysql_stmt_errno(statement);
    (void)_impl->clear_find_result(statement);
    if (is_unavailable_error(error)) {
      _impl->reset_connection();
    }
    throw_mysql_error(error, "MySQL product query result buffering failed");
  }

  std::optional<Product> result;
  try {
    const int fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result == MYSQL_NO_DATA) {
      result = std::nullopt;
    } else if (fetch_result == MYSQL_DATA_TRUNCATED) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    } else if (fetch_result != 0) {
      const auto error = mysql_stmt_errno(statement);
      if (is_unavailable_error(error)) {
        _impl->reset_connection();
      }
      throw_mysql_error(error, "MySQL product row fetch failed");
    } else {
      if (id_is_null || name_is_null || price_is_null || version_is_null || id_error || name_error ||
          price_error || version_error || name_length > max_product_name_bytes ||
          name_length > name_buffer.size()) {
        throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
      }
      product.name.assign(name_buffer.data(), static_cast<std::size_t>(name_length));
      if (product.id != id || !valid_product(product)) {
        throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
      }

      const int next_fetch_result = mysql_stmt_fetch(statement);
      if (next_fetch_result != MYSQL_NO_DATA) {
        if (next_fetch_result != 0 && next_fetch_result != MYSQL_DATA_TRUNCATED) {
          const auto error = mysql_stmt_errno(statement);
          if (is_unavailable_error(error)) {
            _impl->reset_connection();
          }
          throw_mysql_error(error, "MySQL product result drain failed");
        }
        throw StoreError{StoreErrorCode::Unexpected, "MySQL product query returned multiple rows"};
      }
      result = std::move(product);
    }
  } catch (...) {
    if (_impl->find_statement == statement) {
      (void)_impl->clear_find_result(statement);
    }
    throw;
  }

  const auto cleanup_error = _impl->clear_find_result(statement);
  if (cleanup_error != 0) {
    throw_mysql_error(cleanup_error, "MySQL product query cleanup failed");
  }
  return result;
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
