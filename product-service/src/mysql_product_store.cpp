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

// 只有商品服务链接 MySQL 客户端库；C API 的细节只留在本文件。
// StoreError::what() 不包含凭据、SQL 文本或用户输入，避免向上层泄露敏感信息。

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
  // 连接中断、锁等待超时和死锁都属于本次数据库操作不可用，不能当作“查无此行”。
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

    // 懒连接：工作线程首次真正访问 MySQL 时才建连，之后由该线程复用。
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

    // MySQL 8.4 默认不自动重连；MYSQL_OPT_RECONNECT 已弃用，即使设为 false 也会告警。
    // 本模块选择在传输失败后丢弃连接，由下一次独立操作重新建立。
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
    // 数据参数通过占位符绑定，不把商品 ID 拼接进 SQL；语句句柄在本连接内复用。
    constexpr char query[] = "SELECT id, name, price_cents, version FROM products WHERE id = ?";
    return prepare_statement(
        find_statement, query, sizeof(query) - 1, 1, 4, "MySQL statement allocation failed",
        "MySQL product query preparation failed", "MySQL product query shape is invalid");
  }

  MYSQL_STMT* prepare_lock_statement() {
    // FOR UPDATE 在事务中读取当前行并加锁，避免同一商品的并发写入交错执行。
    constexpr char query[] =
        "SELECT id, name, price_cents, version FROM products WHERE id = ? FOR UPDATE";
    return prepare_statement(
        lock_statement, query, sizeof(query) - 1, 1, 4, "MySQL statement allocation failed",
        "MySQL product lock query preparation failed", "MySQL product lock query shape is invalid");
  }

  MYSQL_STMT* prepare_update_statement() {
    // 版本号同时写在更新条件中；更新成功后 version 自增。
    constexpr char query[] =
        "UPDATE products SET name = ?, price_cents = ?, version = version + 1 "
        "WHERE id = ? AND version = ?";
    return prepare_statement(update_statement, query, sizeof(query) - 1, 4, 0,
                             "MySQL statement allocation failed",
                             "MySQL product update preparation failed",
                             "MySQL product update statement shape is invalid");
  }

  /// 清理结果并重置预处理语句以供复用；若清理失败，则关闭整个连接。
  /// 下一个独立操作会从已知的连接状态重新开始。
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
    if (reset_error != 0) {
      return reset_error;
    }
    return free_error != 0 ? free_error : 1;
  }

  [[noreturn]] void throw_statement_error(MYSQL_STMT* statement, const char* message) {
    const auto error = mysql_stmt_errno(statement);
    if (is_connection_failure(error)) {
      reset_connection();
    }
    throw_mysql_error(error, message);
  }

  std::optional<Product> fetch_product_row(MYSQL_STMT* statement, std::uint64_t id) {
    if (mysql_stmt_field_count(statement) != 4) {
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
    results[1].buffer_length = name_buffer.size();
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
    // 只有确实没有结果行才返回 nullopt；截断或字段非法都不能冒充 NotFound。
    if (fetch_result == MYSQL_NO_DATA) {
      return std::nullopt;
    }
    if (fetch_result == MYSQL_DATA_TRUNCATED) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }
    if (fetch_result != 0) {
      throw_statement_error(statement, "MySQL product row fetch failed");
    }
    if (id_is_null || name_is_null || price_is_null || version_is_null || id_error || name_error ||
        price_error || version_error || name_length > max_product_name_bytes ||
        name_length > name_buffer.size()) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }
    product.name.assign(name_buffer.data(), name_length);
    if (product.id != id || !valid_product(product)) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }

    const int next_fetch_result = mysql_stmt_fetch(statement);
    if (next_fetch_result != MYSQL_NO_DATA) {
      if (next_fetch_result != 0 && next_fetch_result != MYSQL_DATA_TRUNCATED) {
        throw_statement_error(statement, "MySQL product result drain failed");
      }
      throw StoreError{StoreErrorCode::Unexpected, "MySQL product query returned multiple rows"};
    }
    return product;
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
      result = fetch_product_row(statement, id);
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

  void execute_versioned_update(MYSQL_STMT* statement, Product& product,
                                std::uint64_t expected_version) {
    unsigned long name_length = product.name.size();
    std::uint64_t price_cents = product.price_cents;
    std::uint64_t product_id = product.id;
    bool name_is_null = false;
    MYSQL_BIND parameters[4]{};
    parameters[0].buffer_type = MYSQL_TYPE_STRING;
    parameters[0].buffer = product.name.data();
    parameters[0].buffer_length = name_length;
    parameters[0].length = &name_length;
    parameters[0].is_null = &name_is_null;
    bind_unsigned_parameter(&parameters[1], &price_cents);
    bind_unsigned_parameter(&parameters[2], &product_id);
    bind_unsigned_parameter(&parameters[3], &expected_version);
    if (mysql_stmt_bind_param(statement, parameters) != 0) {
      throw_statement_error(statement, "MySQL product update parameter binding failed");
    }
    if (mysql_stmt_execute(statement) != 0) {
      throw_statement_error(statement, "MySQL product update failed");
    }
    const auto affected_rows = mysql_stmt_affected_rows(statement);
    const auto cleanup_error = clear_statement_result(update_statement, statement);
    if (cleanup_error != 0) {
      throw_mysql_error(cleanup_error, "MySQL product update cleanup failed");
    }
    if (affected_rows != 1) {
      throw StoreError{StoreErrorCode::Unexpected,
                       "MySQL product update affected an unexpected row count"};
    }
  }

  struct Transaction {
    explicit Transaction(Impl& store) : store{store} {
      constexpr char query[] = "START TRANSACTION";
      if (mysql_real_query(store.connection, query, sizeof(query) - 1) != 0) {
        const auto error = mysql_errno(store.connection);
        if (is_connection_failure(error)) {
          store.reset_connection();
        }
        throw_mysql_error(error, "MySQL transaction start failed");
      }
    }

    ~Transaction() {
      if (active && store.connection != nullptr && mysql_rollback(store.connection) != 0) {
        store.reset_connection();
      }
    }

    void commit() {
      if (mysql_commit(store.connection) != 0) {
        // 可能已经提交，但确认响应丢失；绝不能将此结果当作可重试的回滚。
        store.reset_connection();
        active = false;
        throw StoreError{StoreErrorCode::CommitUnknown, "MySQL commit result is unknown"};
      }
      active = false;
    }

    Impl& store;
    bool active = true;
  };

  void reset_connection() noexcept {
    // 先释放依附于连接的语句句柄，再关闭连接；后续独立请求会重新 prepare。
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
  // 普通查询不加行锁；按主键读取权威商品记录。
  MYSQL_STMT* statement = _impl->prepare_find_statement();
  return _impl->select_product(_impl->find_statement, statement, id);
}

StoreUpdateResult MySqlProductStore::update(const UpdateProductRequest& request) {
  _impl->check_owner();
  if (!valid_update_request(request)) {
    throw std::invalid_argument{"invalid product update request"};
  }

  StoreUpdateResult committed_result;
  committed_result.status = StoreUpdateStatus::Updated;
  committed_result.product =
      Product{request.id, request.name, request.price_cents, request.expected_version + 1};

  _impl->ensure_connection();
  MYSQL_STMT* lock_statement = _impl->prepare_lock_statement();
  MYSQL_STMT* update_statement = _impl->prepare_update_statement();

  // 行锁、版本检查和更新必须在同一事务中；未提交就退出时自动回滚。
  Impl::Transaction transaction{*_impl};
  const auto existing = _impl->select_product(_impl->lock_statement, lock_statement, request.id);
  if (!existing) {
    return {StoreUpdateStatus::NotFound, std::nullopt};
  }
  if (existing->version == std::numeric_limits<std::uint64_t>::max()) {
    throw StoreError{StoreErrorCode::InvalidData, "MySQL product version cannot be advanced"};
  }
  if (existing->version != request.expected_version) {
    return {StoreUpdateStatus::Conflict, std::nullopt};
  }

  _impl->execute_versioned_update(update_statement, *committed_result.product,
                                  request.expected_version);
  transaction.commit();
  return committed_result;
}

}  // namespace sphinx
