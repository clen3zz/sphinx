// SPDX-License-Identifier: Apache-2.0
#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>
#include <sphinx/product/backends/mysql/mysql_product_store.h>
#include <sphinx/product/backends/mysql/mysql_runtime.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// 只有商品服务链接 MySQL 客户端库；C API 的细节只留在本文件。
// StoreError::what() 不包含凭据、SQL 文本或用户输入，避免向上层泄露敏感信息。

namespace sphinx {
namespace {

bool has_embedded_nul(const std::string& value) noexcept {
  return value.find('\0') != std::string::npos;
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

bool is_unavailable_error(unsigned int error) noexcept {
  // 锁等待超时和死锁与连接故障都属于不可用；只有连接故障需要丢弃连接。
  switch (error) {
    case ER_TOO_MANY_USER_CONNECTIONS:
    case ER_LOCK_WAIT_TIMEOUT:
    case ER_LOCK_DEADLOCK:
    case ER_QUERY_TIMEOUT:
      return true;
    default:
      return is_connection_failure(error);
  }
}

void close_statement(MYSQL_STMT*& statement) noexcept {
  if (statement != nullptr) {
    mysql_stmt_close(statement);
    statement = nullptr;
  }
}

StoreErrorCode classify_mysql_error(unsigned int error) noexcept {
  return is_unavailable_error(error) ? StoreErrorCode::Unavailable : StoreErrorCode::Unexpected;
}

[[noreturn]] void throw_mysql_error(unsigned int error, const char* message) {
  throw StoreError{classify_mysql_error(error), message};
}

using MySqlBool = std::remove_pointer_t<decltype(MYSQL_BIND::is_null)>;

void bind_unsigned_result(MYSQL_BIND* binding, std::uint64_t* value, MySqlBool* is_null,
                          MySqlBool* error) {
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

struct ProductRowBuffer final {
  ProductRowBuffer() {
    bind_unsigned_result(&bindings[0], &id, &null_flags[0], &error_flags[0]);
    bindings[1].buffer_type = MYSQL_TYPE_STRING;
    bindings[1].buffer = name.data();
    bindings[1].buffer_length = static_cast<unsigned long>(name.size());
    bindings[1].length = &lengths[1];
    bindings[1].is_null = &null_flags[1];
    bindings[1].error = &error_flags[1];
    bind_unsigned_result(&bindings[2], &price_cents, &null_flags[2], &error_flags[2]);
    bind_unsigned_result(&bindings[3], &version, &null_flags[3], &error_flags[3]);
  }

  ProductRowBuffer(const ProductRowBuffer&) = delete;
  ProductRowBuffer& operator=(const ProductRowBuffer&) = delete;
  ProductRowBuffer(ProductRowBuffer&&) = delete;
  ProductRowBuffer& operator=(ProductRowBuffer&&) = delete;

  void reset_flags() noexcept {
    lengths.fill(0);
    null_flags.fill(MySqlBool{});
    error_flags.fill(MySqlBool{});
  }

  Product to_product() const {
    if (null_flags[0] || null_flags[1] || null_flags[2] || null_flags[3] || error_flags[0] ||
        error_flags[1] || error_flags[2] || error_flags[3] || lengths[1] > max_product_name_bytes ||
        lengths[1] > name.size()) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }

    Product product{id, std::string{name.data(), lengths[1]}, price_cents, version};
    if (!valid_product(product)) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }
    return product;
  }

  std::uint64_t id = 0;
  std::uint64_t price_cents = 0;
  std::uint64_t version = 0;
  std::array<char, max_product_name_bytes + 1> name{};
  std::array<unsigned long, 4> lengths{};
  std::array<MySqlBool, 4> null_flags{};
  std::array<MySqlBool, 4> error_flags{};
  std::array<MYSQL_BIND, 4> bindings{};
};

}  // namespace

void validate_mysql_options(const MySqlOptions& options) {
  if (options.host.empty() || options.user.empty() || options.database.empty() ||
      options.port == 0 || options.connect_timeout_seconds == 0 ||
      options.read_timeout_seconds == 0 || options.write_timeout_seconds == 0 ||
      has_embedded_nul(options.host) || has_embedded_nul(options.user) ||
      has_embedded_nul(options.password) || has_embedded_nul(options.database)) {
    throw std::invalid_argument{"invalid MySQL connection options"};
  }
}

struct MySqlProductStore::Impl {
  explicit Impl(const MySqlOptions& source_options)
      : options{source_options}, owner_thread{std::this_thread::get_id()} {}

  ~Impl() {
    assert(owner_thread == std::this_thread::get_id());
    reset_connection();
  }

  void check_owner() const {
    if (owner_thread != std::this_thread::get_id() || !mysql_thread_initialized()) {
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
                                const char* operation) {
    if (slot != nullptr) {
      return slot;
    }
    MYSQL_STMT* statement = mysql_stmt_init(connection);
    if (statement == nullptr) {
      throw StoreError{StoreErrorCode::Unexpected,
                       std::string{"MySQL "} + operation + " allocation failed"};
    }
    if (mysql_stmt_prepare(statement, query, query_length) != 0) {
      const auto error = mysql_stmt_errno(statement);
      mysql_stmt_close(statement);
      if (is_connection_failure(error)) {
        reset_connection();
      }
      throw StoreError{classify_mysql_error(error),
                       std::string{"MySQL "} + operation + " preparation failed"};
    }
    if (mysql_stmt_param_count(statement) != expected_parameters ||
        mysql_stmt_field_count(statement) != expected_fields) {
      mysql_stmt_close(statement);
      throw StoreError{StoreErrorCode::Unexpected,
                       std::string{"MySQL "} + operation + " shape is invalid"};
    }
    slot = statement;
    return statement;
  }

  MYSQL_STMT* prepare_find_statement() {
    // 数据参数通过占位符绑定，不把商品 ID 拼接进 SQL；语句句柄在本连接内复用。
    constexpr char query[] = "SELECT id, name, price_cents, version FROM products WHERE id = ?";
    return prepare_statement(find_statement, query, sizeof(query) - 1, 1, 4, "product query");
  }

  MYSQL_STMT* prepare_find_many_statement(std::size_t count) {
    if (count == 0 || count > max_product_batch_size) {
      throw std::invalid_argument{"MySQL product batch size must be in 1..32"};
    }
    if (find_many_statement != nullptr && find_many_parameter_count == count) {
      return find_many_statement;
    }
    close_statement(find_many_statement);
    find_many_parameter_count = 0;

    std::string query{"SELECT id, name, price_cents, version FROM products WHERE id IN ("};
    for (std::size_t index = 0; index < count; ++index) {
      if (index != 0) {
        query += ", ";
      }
      query += '?';
    }
    query += ')';
    auto* statement = prepare_statement(find_many_statement, query.c_str(),
                                        static_cast<unsigned long>(query.size()),
                                        static_cast<unsigned int>(count), 4, "product batch query");
    find_many_parameter_count = count;
    return statement;
  }

  MYSQL_STMT* prepare_lock_statement() {
    // FOR UPDATE 在事务中读取当前行并加锁，避免同一商品的并发写入交错执行。
    constexpr char query[] =
        "SELECT id, name, price_cents, version FROM products WHERE id = ? FOR UPDATE";
    return prepare_statement(lock_statement, query, sizeof(query) - 1, 1, 4, "product lock query");
  }

  MYSQL_STMT* prepare_update_statement() {
    // 版本号同时写在更新条件中；更新成功后 version 自增。
    constexpr char query[] =
        "UPDATE products SET name = ?, price_cents = ?, version = version + 1 "
        "WHERE id = ? AND version = ?";
    return prepare_statement(update_statement, query, sizeof(query) - 1, 4, 0, "product update");
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

  std::optional<Product> select_product(MYSQL_STMT*& slot, MYSQL_STMT* statement,
                                        std::uint64_t id) {
    auto products = read_products(slot, statement, {id});
    return std::move(products.front());
  }

  std::vector<std::optional<Product>> select_products(const std::vector<std::uint64_t>& ids) {
    if (ids.empty()) {
      return {};
    }

    std::vector<std::uint64_t> unique_ids;
    unique_ids.reserve(ids.size());
    std::unordered_set<std::uint64_t> requested_ids;
    requested_ids.reserve(ids.size());
    for (const auto id : ids) {
      if (requested_ids.emplace(id).second) {
        unique_ids.push_back(id);
      }
    }

    ensure_connection();
    MYSQL_STMT* statement = prepare_find_many_statement(unique_ids.size());
    return read_products(find_many_statement, statement, ids, std::move(unique_ids));
  }

  // 单条查询、FOR UPDATE 和 IN 查询共用绑定、校验、清理及顺序恢复。
  // parameter_ids 可以去重；ids 保留调用方的原始顺序和重复项。
  std::vector<std::optional<Product>> read_products(MYSQL_STMT*& slot, MYSQL_STMT* statement,
                                                    const std::vector<std::uint64_t>& ids,
                                                    std::vector<std::uint64_t> parameter_ids = {}) {
    if (parameter_ids.empty()) {
      parameter_ids = ids;
    }
    std::vector<MYSQL_BIND> parameters(parameter_ids.size());
    for (std::size_t index = 0; index < parameter_ids.size(); ++index) {
      bind_unsigned_parameter(&parameters[index], &parameter_ids[index]);
    }
    const std::unordered_set<std::uint64_t> requested_ids{ids.begin(), ids.end()};
    std::unordered_map<std::uint64_t, Product> products_by_id;
    products_by_id.reserve(parameter_ids.size());
    try {
      if (mysql_stmt_bind_param(statement, parameters.data()) != 0) {
        throw_statement_error(statement, "MySQL product batch parameter binding failed");
      }
      if (mysql_stmt_execute(statement) != 0) {
        throw_statement_error(statement, "MySQL product batch query failed");
      }
      if (mysql_stmt_field_count(statement) != 4) {
        throw StoreError{StoreErrorCode::Unexpected,
                         "MySQL product batch query returned an invalid shape"};
      }

      ProductRowBuffer row;
      if (mysql_stmt_bind_result(statement, row.bindings.data()) != 0) {
        throw_statement_error(statement, "MySQL product batch result binding failed");
      }
      if (mysql_stmt_store_result(statement) != 0) {
        throw_statement_error(statement, "MySQL product batch result buffering failed");
      }

      while (true) {
        row.reset_flags();
        const int fetch_result = mysql_stmt_fetch(statement);
        if (fetch_result == MYSQL_NO_DATA) {
          break;
        }
        if (fetch_result == MYSQL_DATA_TRUNCATED) {
          throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
        }
        if (fetch_result != 0) {
          throw_statement_error(statement, "MySQL product batch row fetch failed");
        }

        Product product = row.to_product();
        if (requested_ids.find(product.id) == requested_ids.end()) {
          throw StoreError{StoreErrorCode::InvalidData,
                           "MySQL product batch returned an unrequested row"};
        }
        if (!products_by_id.emplace(product.id, std::move(product)).second) {
          throw StoreError{StoreErrorCode::InvalidData,
                           "MySQL product batch returned a duplicate row"};
        }
      }
    } catch (...) {
      if (slot == statement) {
        (void)clear_statement_result(slot, statement);
      }
      throw;
    }

    const auto cleanup_error = clear_statement_result(slot, statement);
    if (cleanup_error != 0) {
      throw_mysql_error(cleanup_error, "MySQL product batch query cleanup failed");
    }

    std::vector<std::optional<Product>> products;
    products.reserve(ids.size());
    for (const auto id : ids) {
      const auto product = products_by_id.find(id);
      if (product == products_by_id.end()) {
        products.emplace_back(std::nullopt);
      } else {
        products.emplace_back(product->second);
      }
    }
    return products;
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
    close_statement(find_statement);
    close_statement(find_many_statement);
    find_many_parameter_count = 0;
    close_statement(lock_statement);
    close_statement(update_statement);
    if (connection != nullptr) {
      mysql_close(connection);
      connection = nullptr;
    }
  }

  MySqlOptions options;
  const std::thread::id owner_thread;
  MYSQL* connection = nullptr;
  MYSQL_STMT* find_statement = nullptr;
  MYSQL_STMT* find_many_statement = nullptr;
  std::size_t find_many_parameter_count = 0;
  MYSQL_STMT* lock_statement = nullptr;
  MYSQL_STMT* update_statement = nullptr;
};

MySqlProductStore::MySqlProductStore(const MySqlOptions& options) : _impl{nullptr} {
  validate_mysql_options(options);
  if (!mysql_thread_initialized()) {
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

std::vector<std::optional<Product>> MySqlProductStore::find_many(
    const std::vector<std::uint64_t>& ids) {
  _impl->check_owner();
  if (ids.size() > max_product_batch_size) {
    throw std::invalid_argument{"product store batch exceeds 32 IDs"};
  }
  for (const auto id : ids) {
    if (id == 0) {
      throw std::invalid_argument{"product store batch IDs must be positive"};
    }
  }
  return _impl->select_products(ids);
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
