// SPDX-License-Identifier: Apache-2.0
#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>
#include <sphinx/product/backends/mysql/mysql_product_store.h>
#include <sphinx/product/backends/mysql/mysql_runtime.h>

#include <algorithm>
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
#include <utility>

// MySQL 客户端依赖只属于存储后端，不进入业务核心。
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

// 绑定借用调用方变量的地址，变量必须存活至 execute/fetch 结束。
using MySqlBool = std::remove_pointer_t<decltype(MYSQL_BIND::is_null)>;
MYSQL_BIND unsigned_binding(std::uint64_t& value, MySqlBool* is_null = nullptr,
                            MySqlBool* error = nullptr) {
  MYSQL_BIND binding{};
  binding.buffer_type = MYSQL_TYPE_LONGLONG;
  binding.buffer = &value;
  binding.is_unsigned = true;
  binding.is_null = is_null;
  binding.error = error;
  return binding;
}

struct ProductRowBuffer {
  ProductRowBuffer() {
    bindings[0] = unsigned_binding(product.id, &nulls[0], &errors[0]);
    bindings[2] = unsigned_binding(product.price_cents, &nulls[2], &errors[2]);
    bindings[3] = unsigned_binding(product.version, &nulls[3], &errors[3]);
    auto& name_binding = bindings[1];
    name_binding.buffer_type = MYSQL_TYPE_STRING;
    name_binding.buffer = name.data();
    name_binding.buffer_length = name.size();
    name_binding.length = &length;
    name_binding.is_null = &nulls[1];
    name_binding.error = &errors[1];
  }
  ProductRowBuffer(const ProductRowBuffer&) = delete;
  ProductRowBuffer& operator=(const ProductRowBuffer&) = delete;

  void reset_flags() {
    length = 0;
    nulls.fill(MySqlBool{});
    errors.fill(MySqlBool{});
  }
  Product to_product() {
    if (std::any_of(nulls.begin(), nulls.end(), [](auto flag) { return flag; }) ||
        std::any_of(errors.begin(), errors.end(), [](auto flag) { return flag; }) ||
        length > max_product_name_bytes) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }
    product.name.assign(name.data(), length);
    if (!valid_product(product)) {
      throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
    }
    return product;
  }
  Product product;
  std::array<char, max_product_name_bytes + 1> name{};
  unsigned long length = 0;
  std::array<MySqlBool, 4> nulls{}, errors{};
  std::array<MYSQL_BIND, 4> bindings{};
};

}  // namespace

struct MySqlProductStore::Connection {
  struct CloseConnection {
    void operator()(MYSQL* handle) const { mysql_close(handle); }
  };
  struct CloseStatement {
    void operator()(MYSQL_STMT* handle) const { mysql_stmt_close(handle); }
  };
  using Statement = std::unique_ptr<MYSQL_STMT, CloseStatement>;
  std::unique_ptr<MYSQL, CloseConnection> handle{mysql_init(nullptr)};
  // 语句先于连接析构；缓存数量由接口规定的批量上限约束。
  std::unordered_map<std::string, Statement> statements;
};

void validate_mysql_options(const MySqlOptions& options) {
  if (options.host.empty() || options.user.empty() || options.database.empty() ||
      options.port == 0 || options.connect_timeout_seconds == 0 ||
      options.read_timeout_seconds == 0 || options.write_timeout_seconds == 0 ||
      has_embedded_nul(options.host) || has_embedded_nul(options.user) ||
      has_embedded_nul(options.password) || has_embedded_nul(options.database)) {
    throw std::invalid_argument{"invalid MySQL connection options"};
  }
}

void MySqlProductStore::check_owner() const {
  if (_owner_thread != std::this_thread::get_id() || !mysql_thread_initialized()) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL store used from a non-owner thread"};
  }
}
void MySqlProductStore::ensure_connection() {
  if (_connection) {
    return;
  }
  auto session = std::make_shared<Connection>();
  MYSQL* handle = session->handle.get();
  if (handle == nullptr) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL connection allocation failed"};
  }
  constexpr unsigned int protocol = MYSQL_PROTOCOL_TCP;
  if (mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &_options.connect_timeout_seconds) ||
      mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &_options.read_timeout_seconds) ||
      mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &_options.write_timeout_seconds) ||
      mysql_options(handle, MYSQL_OPT_PROTOCOL, &protocol)) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL connection option setup failed"};
  }
  if (!mysql_real_connect(handle, _options.host.c_str(), _options.user.c_str(),
                          _options.password.c_str(), _options.database.c_str(), _options.port,
                          nullptr, 0)) {
    throw StoreError{StoreErrorCode::Unavailable, "MySQL connection unavailable"};
  }
  if (mysql_set_character_set(handle, "utf8mb4")) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL character set setup failed"};
  }
  _connection = std::move(session);
}
[[noreturn]] void MySqlProductStore::throw_mysql_error(unsigned int error, const char* message) {
  if (is_connection_failure(error)) {
    _connection.reset();
  }
  throw StoreError{
      is_unavailable_error(error) ? StoreErrorCode::Unavailable : StoreErrorCode::Unexpected,
      message};
}
void MySqlProductStore::check_statement(int result, MYSQL_STMT* statement, const char* message) {
  if (result != 0) {
    throw_mysql_error(mysql_stmt_errno(statement), message);
  }
}
unsigned int MySqlProductStore::clear_statement_result(MYSQL_STMT* statement) {
  const bool free_failed = mysql_stmt_free_result(statement);
  const auto free_error = free_failed ? mysql_stmt_errno(statement) : 0;
  const bool reset_failed = mysql_stmt_reset(statement);
  const auto reset_error = reset_failed ? mysql_stmt_errno(statement) : 0;
  if (!free_failed && !reset_failed) {
    return 0;
  }
  _connection.reset();
  if (reset_error != 0) {
    return reset_error;
  }
  return free_error != 0 ? free_error : 1;
}

template <typename Operation>
auto MySqlProductStore::execute_statement(const std::string& sql, MYSQL_BIND* parameters,
                                          Operation operation) {
  ensure_connection();
  // 出错时连接可能从 store 移除；局部引用保证语句清理前连接仍存活。
  auto session = _connection;
  auto& cached = session->statements[sql];
  if (!cached) {
    Connection::Statement statement{mysql_stmt_init(session->handle.get())};
    if (!statement) {
      throw StoreError{StoreErrorCode::Unexpected, "MySQL statement allocation failed"};
    }
    check_statement(mysql_stmt_prepare(statement.get(), sql.data(), sql.size()), statement.get(),
                    "MySQL statement preparation failed");
    cached = std::move(statement);
  }
  MYSQL_STMT* statement = cached.get();
  try {
    check_statement(mysql_stmt_bind_param(statement, parameters), statement,
                    "MySQL parameter binding failed");
    check_statement(mysql_stmt_execute(statement), statement, "MySQL query failed");
    auto result = operation(statement);
    const auto error = clear_statement_result(statement);
    if (error) {
      throw_mysql_error(error, "MySQL query cleanup failed");
    }
    return result;
  } catch (...) {
    if (_connection == session) {
      (void)clear_statement_result(statement);
    }
    throw;
  }
}

std::vector<std::optional<Product>> MySqlProductStore::read_products(std::vector<std::uint64_t> ids,
                                                                     bool for_update) {
  if (ids.empty()) {
    return {};
  }
  std::vector<MYSQL_BIND> parameters;
  parameters.reserve(ids.size());
  for (auto& id : ids) {
    parameters.push_back(unsigned_binding(id));
  }
  std::string sql = "SELECT id, name, price_cents, version FROM products WHERE id IN (";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    sql += index ? ",?" : "?";
  }
  sql += for_update ? ") FOR UPDATE" : ")";
  return execute_statement(sql, parameters.data(), [&](MYSQL_STMT* statement) {
    ProductRowBuffer row;
    check_statement(mysql_stmt_bind_result(statement, row.bindings.data()), statement,
                    "MySQL result binding failed");
    check_statement(mysql_stmt_store_result(statement), statement, "MySQL result buffering failed");
    std::unordered_map<std::uint64_t, Product> found;
    for (;;) {
      row.reset_flags();
      const auto status = mysql_stmt_fetch(statement);
      if (status == MYSQL_NO_DATA) {
        break;
      }
      if (status == MYSQL_DATA_TRUNCATED) {
        throw StoreError{StoreErrorCode::InvalidData, "MySQL product row is invalid"};
      }
      check_statement(status, statement, "MySQL row fetch failed");
      auto product = row.to_product();
      found.emplace(product.id, std::move(product));
    }
    std::vector<std::optional<Product>> products;
    products.reserve(ids.size());
    for (auto id : ids) {
      const auto position = found.find(id);
      products.push_back(position == found.end() ? std::nullopt : std::optional{position->second});
    }
    return products;
  });
}

struct MySqlProductStore::Transaction {
  explicit Transaction(MySqlProductStore& store) : store{store} {
    store.ensure_connection();
    if (mysql_query(store._connection->handle.get(), "START TRANSACTION")) {
      store.throw_mysql_error(mysql_errno(store._connection->handle.get()),
                              "MySQL transaction start failed");
    }
  }
  ~Transaction() {
    if (active && store._connection && mysql_rollback(store._connection->handle.get())) {
      store._connection.reset();
    }
  }
  void commit() {
    if (mysql_commit(store._connection->handle.get())) {
      store._connection.reset();
      active = false;
      throw StoreError{StoreErrorCode::CommitUnknown, "MySQL commit result is unknown"};
    }
    active = false;
  }
  MySqlProductStore& store;
  bool active = true;
};

MySqlProductStore::MySqlProductStore(const MySqlOptions& options) : _options{options} {
  validate_mysql_options(options);
  if (!mysql_thread_initialized()) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL store requires a thread guard"};
  }
}
MySqlProductStore::~MySqlProductStore() { assert(_owner_thread == std::this_thread::get_id()); }

std::optional<Product> MySqlProductStore::find(std::uint64_t id) {
  check_owner();
  if (id == 0) {
    throw std::invalid_argument{"product id must be positive"};
  }
  return std::move(read_products({id}).front());
}
std::vector<std::optional<Product>> MySqlProductStore::find_many(
    const std::vector<std::uint64_t>& ids) {
  check_owner();
  if (ids.size() > max_product_batch_size || std::find(ids.begin(), ids.end(), 0) != ids.end()) {
    throw std::invalid_argument{"invalid product store batch IDs"};
  }
  return read_products(ids);
}
StoreUpdateResult MySqlProductStore::update(const UpdateProductRequest& request) {
  check_owner();
  if (!valid_update_request(request)) {
    throw std::invalid_argument{"invalid product update request"};
  }
  Transaction transaction{*this};
  const auto existing = read_products({request.id}, true).front();
  if (!existing) {
    return {StoreUpdateStatus::NotFound, std::nullopt};
  }
  if (existing->version == std::numeric_limits<std::uint64_t>::max()) {
    throw StoreError{StoreErrorCode::InvalidData, "MySQL product version cannot be advanced"};
  }
  if (existing->version != request.expected_version) {
    return {StoreUpdateStatus::Conflict, std::nullopt};
  }
  Product product{request.id, request.name, request.price_cents, request.expected_version + 1};
  unsigned long name_length = product.name.size();
  MYSQL_BIND name{};
  name.buffer_type = MYSQL_TYPE_STRING;
  name.buffer = product.name.data();
  name.buffer_length = name_length;
  name.length = &name_length;
  auto expected_version = request.expected_version;
  std::array<MYSQL_BIND, 4> parameters{name, unsigned_binding(product.price_cents),
                                       unsigned_binding(product.id),
                                       unsigned_binding(expected_version)};
  const auto affected = execute_statement(
      "UPDATE products SET name = ?, price_cents = ?, version = version + 1 WHERE id = ? AND "
      "version = ?",
      parameters.data(), [](MYSQL_STMT* statement) { return mysql_stmt_affected_rows(statement); });
  if (affected != 1) {
    throw StoreError{StoreErrorCode::Unexpected, "MySQL update affected an unexpected row count"};
  }
  transaction.commit();
  return {StoreUpdateStatus::Updated, std::move(product)};
}

}  // namespace sphinx
