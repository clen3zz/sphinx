// SPDX-License-Identifier: Apache-2.0
#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>
#include <sphinx/mysql_product_store.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

// This optional target is wired to libmysqlclient. Keep the MySQL C API private to this file, and
// keep credentials, query text, and user input out of StoreError::what().

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

bool is_connection_failure(unsigned int error) noexcept {
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

void bind_unsigned_parameter(MYSQL_BIND* binding, std::uint64_t* value) {
  binding->buffer_type = MYSQL_TYPE_LONGLONG;
  binding->buffer = value;
  binding->is_unsigned = true;
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

    const unsigned int connect_timeout{options.connect_timeout_seconds};
    const unsigned int read_timeout{options.read_timeout_seconds};
    const unsigned int write_timeout{options.write_timeout_seconds};
    constexpr unsigned int protocol = MYSQL_PROTOCOL_TCP;
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

  MYSQL_STMT* prepare_statement(MYSQL_STMT*& slot, const char* query, unsigned long query_length,
                                unsigned int expected_parameters, unsigned int expected_fields,
                                const char* allocation_error, const char* preparation_error,
                                const char* shape_error) {
    if (slot != nullptr) {
      return slot;
    }
    MYSQL_STMT* statement = mysql_stmt_init(connection);
    if (statement == nullptr) {
      throw StoreError{StoreErrorCode::Unexpected, allocation_error};
    }
    if (mysql_stmt_prepare(statement, query, query_length) != 0) {
      const auto error = mysql_stmt_errno(statement);
      mysql_stmt_close(statement);
      if (is_connection_failure(error)) {
        reset_connection();
      }
      throw_mysql_error(error, preparation_error);
    }
    if (mysql_stmt_param_count(statement) != expected_parameters ||
        mysql_stmt_field_count(statement) != expected_fields) {
      mysql_stmt_close(statement);
      throw StoreError{StoreErrorCode::Unexpected, shape_error};
    }
    slot = statement;
    return statement;
  }

  MYSQL_STMT* prepare_find_statement() {
    constexpr char query[] = "SELECT id, name, price_cents, version FROM products WHERE id = ?";
    return prepare_statement(
        find_statement, query, sizeof(query) - 1, 1, 4, "MySQL statement allocation failed",
        "MySQL product query preparation failed", "MySQL product query shape is invalid");
  }

  MYSQL_STMT* prepare_lock_statement() {
    constexpr char query[] =
        "SELECT id, name, price_cents, version FROM products WHERE id = ? FOR UPDATE";
    return prepare_statement(
        lock_statement, query, sizeof(query) - 1, 1, 4, "MySQL statement allocation failed",
        "MySQL product lock query preparation failed", "MySQL product lock query shape is invalid");
  }

  MYSQL_STMT* prepare_update_statement() {
    constexpr char query[] =
        "UPDATE products SET name = ?, price_cents = ?, version = version + 1 "
        "WHERE id = ? AND version = ?";
    return prepare_statement(update_statement, query, sizeof(query) - 1, 4, 0,
                             "MySQL statement allocation failed",
                             "MySQL product update preparation failed",
                             "MySQL product update statement shape is invalid");
  }

  /// Clear buffered rows and reset a prepared statement for reuse. On failure, close the whole
  /// connection so the next independent operation starts from a known state.
  unsigned int clear_statement_result(MYSQL_STMT*& slot, MYSQL_STMT* statement) noexcept {
    if (slot != statement || connection == nullptr) {
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

  [[noreturn]] void throw_statement_error(MYSQL_STMT* statement, const char* message) {
    const auto error = mysql_stmt_errno(statement);
    if (is_connection_failure(error)) {
      reset_connection();
    }
    throw_mysql_error(error, message);
  }

  std::optional<Product> select_product(MYSQL_STMT*& slot, MYSQL_STMT* statement,
                                        std::uint64_t id) {
    std::uint64_t requested_id = id;
    MYSQL_BIND parameter{};
    bind_unsigned_parameter(&parameter, &requested_id);
    if (mysql_stmt_bind_param(statement, &parameter) != 0) {
      throw_statement_error(statement, "MySQL product query parameter binding failed");
    }
    if (mysql_stmt_execute(statement) != 0) {
      throw_statement_error(statement, "MySQL product query failed");
    }

    std::optional<Product> result;
    try {
      if (mysql_stmt_field_count(statement) != 4) {
        throw StoreError{StoreErrorCode::Unexpected,
                         "MySQL product query returned an invalid shape"};
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
        throw_statement_error(statement, "MySQL product query result binding failed");
      }
      if (mysql_stmt_store_result(statement) != 0) {
        throw_statement_error(statement, "MySQL product query result buffering failed");
      }

      const int fetch_result = mysql_stmt_fetch(statement);
      if (fetch_result == MYSQL_NO_DATA) {
        result = std::nullopt;
      } else if (fetch_result == MYSQL_DATA_TRUNCATED) {
        throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
      } else if (fetch_result != 0) {
        throw_statement_error(statement, "MySQL product row fetch failed");
      } else {
        if (id_is_null || name_is_null || price_is_null || version_is_null || id_error ||
            name_error || price_error || version_error || name_length > max_product_name_bytes ||
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
            throw_statement_error(statement, "MySQL product result drain failed");
          }
          throw StoreError{StoreErrorCode::Unexpected,
                           "MySQL product query returned multiple rows"};
        }
        result = std::move(product);
      }
    } catch (...) {
      if (slot == statement) {
        (void)clear_statement_result(slot, statement);
      }
      throw;
    }

    const auto cleanup_error = clear_statement_result(slot, statement);
    if (cleanup_error != 0) {
      throw_mysql_error(cleanup_error, "MySQL product query cleanup failed");
    }
    return result;
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
  return _impl->select_product(_impl->find_statement, statement, id);
}

StoreUpdateResult MySqlProductStore::update(const UpdateProductRequest& request) {
  _impl->check_owner();
  if (request.id == 0 || request.expected_version == 0 ||
      request.expected_version == std::numeric_limits<std::uint64_t>::max() ||
      !valid_product(Product{request.id, request.name, request.price_cents, 1})) {
    throw std::invalid_argument{"invalid product update request"};
  }

  StoreUpdateResult committed_result;
  committed_result.status = StoreUpdateStatus::Updated;
  committed_result.product =
      Product{request.id, request.name, request.price_cents, request.expected_version + 1};

  _impl->ensure_connection();
  MYSQL_STMT* lock_statement = _impl->prepare_lock_statement();
  MYSQL_STMT* update_statement = _impl->prepare_update_statement();

  constexpr char start_transaction[] = "START TRANSACTION";
  if (mysql_real_query(_impl->connection, start_transaction, sizeof(start_transaction) - 1) != 0) {
    const auto error = mysql_errno(_impl->connection);
    if (is_connection_failure(error)) {
      _impl->reset_connection();
    }
    throw_mysql_error(error, "MySQL transaction start failed");
  }
  bool transaction_active = true;
  const auto rollback = [&]() noexcept {
    if (!transaction_active) {
      return;
    }
    transaction_active = false;
    if (_impl->connection != nullptr && mysql_rollback(_impl->connection) != 0) {
      _impl->reset_connection();
    }
  };

  try {
    const auto existing = _impl->select_product(_impl->lock_statement, lock_statement, request.id);
    if (!existing) {
      rollback();
      return {StoreUpdateStatus::NotFound, std::nullopt};
    }
    if (existing->version == std::numeric_limits<std::uint64_t>::max()) {
      rollback();
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product version cannot be advanced"};
    }
    if (existing->version != request.expected_version) {
      rollback();
      return {StoreUpdateStatus::Conflict, std::nullopt};
    }

    auto& new_product = *committed_result.product;
    unsigned long name_length = static_cast<unsigned long>(new_product.name.size());
    std::uint64_t price_cents = new_product.price_cents;
    std::uint64_t product_id = new_product.id;
    std::uint64_t expected_version = request.expected_version;
    bool name_is_null = false;
    MYSQL_BIND parameters[4]{};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = new_product.name.data();
    parameters[0].buffer_length = name_length;
    parameters[0].length = &name_length;
    parameters[0].is_null = &name_is_null;
    bind_unsigned_parameter(&parameters[1], &price_cents);
    bind_unsigned_parameter(&parameters[2], &product_id);
    bind_unsigned_parameter(&parameters[3], &expected_version);
    if (mysql_stmt_bind_param(update_statement, parameters) != 0) {
      _impl->throw_statement_error(update_statement,
                                   "MySQL product update parameter binding failed");
    }
    if (mysql_stmt_execute(update_statement) != 0) {
      _impl->throw_statement_error(update_statement, "MySQL product update failed");
    }
    const auto affected_rows = mysql_stmt_affected_rows(update_statement);
    const auto cleanup_error =
        _impl->clear_statement_result(_impl->update_statement, update_statement);
    if (cleanup_error != 0) {
      throw_mysql_error(cleanup_error, "MySQL product update cleanup failed");
    }
    if (affected_rows != 1) {
      throw StoreError{StoreErrorCode::Unexpected,
                       "MySQL product update affected an unexpected row count"};
    }
  } catch (...) {
    rollback();
    throw;
  }

  if (mysql_commit(_impl->connection) != 0) {
    _impl->reset_connection();
    throw StoreError{StoreErrorCode::CommitUnknown, "MySQL commit result is unknown"};
  }
  transaction_active = false;
  return committed_result;
}

}  // namespace sphinx
