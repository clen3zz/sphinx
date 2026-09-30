// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <mysql.h>
#include <sphinx/mysql_product_store.h>

#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> inject_error_after_real_commit{false};
std::atomic<int> commit_calls{0};

std::optional<sphinx::MySqlOptions> read_test_options() {
  // 测试配置在所有工作线程启动前读取。
  // NOLINTBEGIN(concurrency-mt-unsafe)
  const auto* host = std::getenv("SPHINX_TEST_MYSQL_HOST");
  const auto* port_text = std::getenv("SPHINX_TEST_MYSQL_PORT");
  const auto* user = std::getenv("SPHINX_TEST_MYSQL_USER");
  const auto* password = std::getenv("SPHINX_TEST_MYSQL_PASSWORD");
  const auto* database = std::getenv("SPHINX_TEST_MYSQL_DATABASE");
  // NOLINTEND(concurrency-mt-unsafe)
  if (host == nullptr || port_text == nullptr || user == nullptr || password == nullptr ||
      database == nullptr || host[0] == '\0' || user[0] == '\0' || database[0] == '\0' ||
      std::string_view{database}.find("test") == std::string_view::npos) {
    return std::nullopt;
  }

  const std::string_view port_value{port_text};
  std::uint64_t port = 0;
  const auto parsed =
      std::from_chars(port_value.data(), port_value.data() + port_value.size(), port);
  if (port_value.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != port_value.data() + port_value.size() || port == 0 || port > 65535) {
    throw std::invalid_argument{"SPHINX_TEST_MYSQL_PORT is invalid"};
  }
  return sphinx::MySqlOptions{host, static_cast<std::uint16_t>(port), user, password, database};
}

void require_mysql(bool failed, const char* message) {
  if (failed) {
    throw std::runtime_error{message};
  }
}

class TestStatement final {
 public:
  explicit TestStatement(MYSQL* connection) : _statement{mysql_stmt_init(connection)} {
    require_mysql(_statement == nullptr, "test MySQL statement allocation failed");
  }
  ~TestStatement() { mysql_stmt_close(_statement); }
  TestStatement(const TestStatement&) = delete;
  TestStatement& operator=(const TestStatement&) = delete;

  MYSQL_STMT* get() const noexcept { return _statement; }

  void prepare(const char* query, unsigned long length) {
    require_mysql(mysql_stmt_prepare(_statement, query, length) != 0,
                  "test MySQL statement preparation failed");
  }

 private:
  MYSQL_STMT* _statement;
};

void bind_unsigned(MYSQL_BIND* binding, std::uint64_t* value) {
  binding->buffer_type = MYSQL_TYPE_LONGLONG;
  binding->buffer = value;
  binding->is_unsigned = true;
}

class TestDatabase final {
 public:
  explicit TestDatabase(const sphinx::MySqlOptions& options) {
    MYSQL* connection = mysql_init(nullptr);
    require_mysql(connection == nullptr, "test MySQL connection allocation failed");

    const unsigned int timeout = options.connect_timeout_seconds;
    const unsigned int protocol = MYSQL_PROTOCOL_TCP;
    if (mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection, MYSQL_OPT_PROTOCOL, &protocol) != 0 ||
        mysql_real_connect(connection, options.host.c_str(), options.user.c_str(),
                           options.password.c_str(), options.database.c_str(), options.port,
                           nullptr, 0) == nullptr) {
      mysql_close(connection);
      throw std::runtime_error{"test MySQL connection failed"};
    }
    _connection = connection;
    if (mysql_set_character_set(_connection, "utf8mb4") != 0) {
      mysql_close(_connection);
      _connection = nullptr;
      throw std::runtime_error{"test MySQL character set setup failed"};
    }
    try {
      execute("SET SESSION lock_wait_timeout = 3");
    } catch (...) {
      mysql_close(_connection);
      _connection = nullptr;
      throw;
    }
  }

  ~TestDatabase() {
    if (_connection != nullptr) {
      mysql_close(_connection);
    }
  }
  TestDatabase(const TestDatabase&) = delete;
  TestDatabase& operator=(const TestDatabase&) = delete;

  void execute(const char* query) {
    if (mysql_real_query(_connection, query, std::char_traits<char>::length(query)) != 0) {
      throw std::runtime_error{std::string{"test MySQL fixture statement failed: "} +
                               mysql_error(_connection)};
    }
  }

  void erase(std::uint64_t id) {
    constexpr char query[] = "DELETE FROM products WHERE id = ?";
    TestStatement statement{_connection};
    statement.prepare(query, sizeof(query) - 1);
    MYSQL_BIND parameters[1]{};
    bind_unsigned(&parameters[0], &id);
    require_mysql(mysql_stmt_bind_param(statement.get(), parameters) != 0,
                  "test MySQL fixture delete binding failed");
    require_mysql(mysql_stmt_execute(statement.get()) != 0, "test MySQL fixture delete failed");
  }

  void insert(const sphinx::Product& product) {
    constexpr char query[] =
        "INSERT INTO products (id, name, price_cents, version) VALUES (?, ?, ?, ?)";
    TestStatement statement{_connection};
    statement.prepare(query, sizeof(query) - 1);
    auto name = product.name;
    unsigned long name_length = static_cast<unsigned long>(name.size());
    std::uint64_t id = product.id;
    std::uint64_t price_cents = product.price_cents;
    std::uint64_t version = product.version;
    bool name_is_null = false;
    MYSQL_BIND parameters[4]{};
    parameters[0].buffer_type = MYSQL_TYPE_LONGLONG;
    parameters[0].buffer = &id;
    parameters[0].is_unsigned = true;
    parameters[1].buffer_type = MYSQL_TYPE_STRING;
    parameters[1].buffer = name.data();
    parameters[1].buffer_length = name_length;
    parameters[1].length = &name_length;
    parameters[1].is_null = &name_is_null;
    bind_unsigned(&parameters[2], &price_cents);
    bind_unsigned(&parameters[3], &version);
    require_mysql(mysql_stmt_bind_param(statement.get(), parameters) != 0,
                  "test MySQL fixture insert binding failed");
    require_mysql(mysql_stmt_execute(statement.get()) != 0, "test MySQL fixture insert failed");
  }

  void insert_null_name(std::uint64_t id) {
    constexpr char query[] =
        "INSERT INTO products (id, name, price_cents, version) VALUES (?, ?, ?, ?)";
    TestStatement statement{_connection};
    statement.prepare(query, sizeof(query) - 1);
    std::uint64_t price_cents = 1;
    std::uint64_t version = 1;
    bool name_is_null = true;
    MYSQL_BIND parameters[4]{};
    bind_unsigned(&parameters[0], &id);
    parameters[1].buffer_type = MYSQL_TYPE_NULL;
    parameters[1].is_null = &name_is_null;
    bind_unsigned(&parameters[2], &price_cents);
    bind_unsigned(&parameters[3], &version);
    require_mysql(mysql_stmt_bind_param(statement.get(), parameters) != 0,
                  "test MySQL null fixture binding failed");
    require_mysql(mysql_stmt_execute(statement.get()) != 0,
                  "test MySQL null fixture insert failed");
  }

 private:
  MYSQL* _connection = nullptr;
};

class ProductSchemaGuard final {
 public:
  explicit ProductSchemaGuard(TestDatabase& database) : _database{database} {
    try {
      _database.execute("ALTER TABLE products DROP CHECK chk_products_name_bytes");
      _name_check_removed = true;
      _database.execute("ALTER TABLE products DROP CHECK chk_products_price");
      _price_check_removed = true;
      _database.execute("ALTER TABLE products MODIFY name VARCHAR(128) NULL");
      _name_nullable = true;
    } catch (...) {
      try {
        restore();
      } catch (...) {
        ADD_FAILURE() << "failed to restore product schema after setup error";
      }
      throw;
    }
  }

  ~ProductSchemaGuard() {
    try {
      restore();
    } catch (...) {
      ADD_FAILURE() << "failed to restore product schema";
    }
  }
  ProductSchemaGuard(const ProductSchemaGuard&) = delete;
  ProductSchemaGuard& operator=(const ProductSchemaGuard&) = delete;

  void restore() {
    if (_name_nullable) {
      _database.execute("ALTER TABLE products MODIFY name VARCHAR(128) NOT NULL");
      _name_nullable = false;
    }
    if (_name_check_removed) {
      _database.execute(
          "ALTER TABLE products ADD CONSTRAINT chk_products_name_bytes "
          "CHECK (OCTET_LENGTH(name) BETWEEN 1 AND 128)");
      _name_check_removed = false;
    }
    if (_price_check_removed) {
      _database.execute(
          "ALTER TABLE products ADD CONSTRAINT chk_products_price "
          "CHECK (price_cents <= 1000000000000)");
      _price_check_removed = false;
    }
  }

 private:
  TestDatabase& _database;
  bool _name_check_removed = false;
  bool _price_check_removed = false;
  bool _name_nullable = false;
};

class FixtureRows final {
 public:
  FixtureRows(TestDatabase& database, std::initializer_list<std::uint64_t> ids)
      : _database{database}, _ids{ids} {
    for (const auto id : _ids) {
      _database.erase(id);
    }
  }

  ~FixtureRows() { erase_all_noexcept(); }
  FixtureRows(const FixtureRows&) = delete;
  FixtureRows& operator=(const FixtureRows&) = delete;

  void insert(const sphinx::Product& product) { _database.insert(product); }
  void insert_null_name(std::uint64_t id) { _database.insert_null_name(id); }

  void erase_all() {
    for (const auto id : _ids) {
      _database.erase(id);
    }
    _ids.clear();
  }

 private:
  void erase_all_noexcept() noexcept {
    for (const auto id : _ids) {
      try {
        _database.erase(id);
      } catch (...) {
        ADD_FAILURE() << "failed to erase test product " << id;
      }
    }
  }

  TestDatabase& _database;
  std::vector<std::uint64_t> _ids;
};

class UpdateFailureConstraint final {
 public:
  explicit UpdateFailureConstraint(TestDatabase& database) : _database{database} {
    _database.execute(
        "ALTER TABLE products ADD CONSTRAINT test_reject_price "
        "CHECK (id <> 73001 OR price_cents < 2)");
    _installed = true;
  }
  ~UpdateFailureConstraint() { drop_noexcept(); }
  UpdateFailureConstraint(const UpdateFailureConstraint&) = delete;
  UpdateFailureConstraint& operator=(const UpdateFailureConstraint&) = delete;

  void drop() {
    if (_installed) {
      _database.execute("ALTER TABLE products DROP CHECK test_reject_price");
      _installed = false;
    }
  }

 private:
  void drop_noexcept() noexcept {
    try {
      drop();
    } catch (...) {
      ADD_FAILURE() << "failed to remove test constraint";
    }
  }

  TestDatabase& _database;
  bool _installed = false;
};

}  // namespace

using CommitReturn = decltype(mysql_commit(static_cast<MYSQL*>(nullptr)));
// GNU ld 的 --wrap 选项要求这些 C 符号名称保持原样。
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" CommitReturn __real_mysql_commit(MYSQL* connection);
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" CommitReturn __wrap_mysql_commit(MYSQL* connection) {
  commit_calls.fetch_add(1);
  const auto result = __real_mysql_commit(connection);
  if (result == 0 && inject_error_after_real_commit.exchange(false)) {
    return static_cast<CommitReturn>(1);
  }
  return result;
}

TEST(MySqlRuntimeTest, InitializesBeforeWorkerThreadAndAllowsSequentialLifetimes) {
  {
    sphinx::MySqlRuntime runtime;
    {
      sphinx::MySqlThreadGuard worker;
    }
  }
  {
    sphinx::MySqlRuntime runtime;
    sphinx::MySqlThreadGuard worker;
  }
}

TEST(MySqlRuntimeTest, StoreRequiresWorkerGuardAndValidOptions) {
  sphinx::MySqlRuntime runtime;
  const sphinx::MySqlOptions options{"127.0.0.1", 3306, "test", "", "sphinx_test"};
  EXPECT_THROW(sphinx::MySqlProductStore store{options}, sphinx::StoreError);
  sphinx::MySqlThreadGuard worker;
  {
    sphinx::MySqlProductStore store{options};
    EXPECT_TRUE(store.find_many({}).empty());
    EXPECT_THROW(store.find_many({1, 0}), std::invalid_argument);
    const std::vector<std::uint64_t> oversized(sphinx::max_product_batch_size + 1, 1);
    EXPECT_THROW(store.find_many(oversized), std::invalid_argument);
  }
  auto invalid_options = options;
  invalid_options.host.clear();
  EXPECT_THROW(sphinx::MySqlProductStore store{invalid_options}, std::invalid_argument);
}

TEST(MySqlProductStoreIntegrationTest, ReadsExistingMissingAndInvalidProducts) {
  const auto options = read_test_options();
  if (!options) {
    GTEST_SKIP()
        << "requires SPHINX_TEST_MYSQL_* for a disposable database whose name includes test";
  }
  sphinx::MySqlRuntime runtime;
  sphinx::MySqlThreadGuard worker;
  TestDatabase database{*options};
  ProductSchemaGuard schema{database};
  constexpr std::uint64_t max_id = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t first_invalid_id = 71001;
  FixtureRows rows{database,
                   {max_id, max_id - 1, first_invalid_id, first_invalid_id + 1,
                    first_invalid_id + 2, first_invalid_id + 3, first_invalid_id + 4}};
  const sphinx::Product expected{max_id, u8"边界商品🌱", sphinx::max_product_price_cents, max_id};
  rows.insert(expected);
  rows.insert({first_invalid_id, std::string{1, '\x01'}, 1, 1});
  rows.insert({first_invalid_id + 1, std::string(126, 'n') + u8"中", 1, 1});
  rows.insert({first_invalid_id + 2, std::string(125, 't') + u8"中" + "ab", 1, 1});
  rows.insert({first_invalid_id + 3, "too expensive", sphinx::max_product_price_cents + 1, 1});
  rows.insert_null_name(first_invalid_id + 4);

  {
    sphinx::MySqlProductStore store{*options};
    const auto batch = store.find_many({max_id, max_id - 1, max_id});
    EXPECT_EQ(batch.size(), 3U);
    if (batch.size() == 3) {
      EXPECT_TRUE(batch[0]);
      EXPECT_EQ(batch[0].value_or(sphinx::Product{}).id, expected.id);
      EXPECT_FALSE(batch[1]);
      EXPECT_TRUE(batch[2]);
      EXPECT_EQ(batch[2].value_or(sphinx::Product{}).id, expected.id);
    }

    const auto actual = store.find(max_id);
    if (!actual) {
      ADD_FAILURE() << "existing product was not found";
      return;
    }
    EXPECT_EQ(actual->id, expected.id);
    EXPECT_EQ(actual->name, expected.name);
    EXPECT_EQ(actual->price_cents, expected.price_cents);
    EXPECT_EQ(actual->version, expected.version);
    EXPECT_FALSE(store.find(max_id - 1).has_value());

    for (const auto id : {first_invalid_id, first_invalid_id + 1, first_invalid_id + 2,
                          first_invalid_id + 3, first_invalid_id + 4}) {
      SCOPED_TRACE(id);
      try {
        (void)store.find(id);
        FAIL() << "invalid database row was accepted";
      } catch (const sphinx::StoreError& error) {
        EXPECT_EQ(error.code(), sphinx::StoreErrorCode::InvalidData);
      }
    }
    try {
      (void)store.find_many({max_id, first_invalid_id});
      FAIL() << "invalid row in batch was accepted";
    } catch (const sphinx::StoreError& error) {
      EXPECT_EQ(error.code(), sphinx::StoreErrorCode::InvalidData);
    }
    EXPECT_EQ(store.find_many({max_id}).size(), 1U);
    EXPECT_EQ(store.find_many({max_id, max_id - 1}).size(), 2U);
    EXPECT_THROW(store.find(0), std::invalid_argument);
    EXPECT_THROW(store.find_many({max_id, 0}), std::invalid_argument);
  }

  rows.erase_all();
  EXPECT_NO_THROW(schema.restore());
}

TEST(MySqlProductStoreIntegrationTest, SerializesConcurrentVersionUpdates) {
  // 两个独立连接同时按版本 1 更新同一行：应只有一个提交，另一个报告版本冲突。
  const auto options = read_test_options();
  if (!options) {
    GTEST_SKIP()
        << "requires SPHINX_TEST_MYSQL_* for a disposable database whose name includes test";
  }
  sphinx::MySqlRuntime runtime;
  sphinx::MySqlThreadGuard main_worker;
  TestDatabase database{*options};
  constexpr std::uint64_t product_id = 72001;
  FixtureRows rows{database, {product_id}};
  rows.insert({product_id, "concurrent", 1, 1});

  std::mutex mutex;
  std::condition_variable condition;
  int ready = 0;
  bool start = false;
  std::array<sphinx::StoreUpdateStatus, 2> results{sphinx::StoreUpdateStatus::NotFound,
                                                   sphinx::StoreUpdateStatus::NotFound};
  std::array<bool, 2> failures{false, false};
  std::array<std::thread, 2> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread{[&, index] {
      bool reached_barrier = false;
      try {
        sphinx::MySqlThreadGuard worker;
        sphinx::MySqlProductStore store{*options};
        {
          std::unique_lock<std::mutex> lock{mutex};
          ++ready;
          reached_barrier = true;
          condition.notify_all();
          condition.wait(lock, [&] { return start; });
        }
        results[index] = store.update({product_id, "winner", 2, 1}).status;
      } catch (...) {
        failures[index] = true;
        if (!reached_barrier) {
          std::lock_guard<std::mutex> lock{mutex};
          ++ready;
          condition.notify_all();
        }
      }
    }};
  }
  {
    std::unique_lock<std::mutex> lock{mutex};
    condition.wait(lock, [&] { return ready == 2; });
    start = true;
  }
  condition.notify_all();
  for (auto& thread : workers) {
    thread.join();
  }

  EXPECT_FALSE(failures[0]);
  EXPECT_FALSE(failures[1]);
  const auto updated_count = static_cast<int>(results[0] == sphinx::StoreUpdateStatus::Updated) +
                             static_cast<int>(results[1] == sphinx::StoreUpdateStatus::Updated);
  const auto conflict_count = static_cast<int>(results[0] == sphinx::StoreUpdateStatus::Conflict) +
                              static_cast<int>(results[1] == sphinx::StoreUpdateStatus::Conflict);
  EXPECT_EQ(updated_count, 1);
  EXPECT_EQ(conflict_count, 1);

  sphinx::MySqlProductStore reader{*options};
  const auto product = reader.find(product_id);
  if (!product) {
    ADD_FAILURE() << "concurrent update left no product";
    return;
  }
  EXPECT_EQ(product->version, 2);
  EXPECT_EQ(product->name, "winner");
}

TEST(MySqlProductStoreIntegrationTest, RollsBackKnownFailureAndDoesNotRetryUnknownCommit) {
  // 已知失败必须回滚；模拟“实际提交成功但确认响应失败”，验证不会盲目重试。
  const auto options = read_test_options();
  if (!options) {
    GTEST_SKIP()
        << "requires SPHINX_TEST_MYSQL_* for a disposable database whose name includes test";
  }
  sphinx::MySqlRuntime runtime;
  sphinx::MySqlThreadGuard main_worker;
  TestDatabase database{*options};
  constexpr std::uint64_t product_id = 73001;
  FixtureRows rows{database, {product_id}};
  rows.insert({product_id, "rollback", 1, 1});
  UpdateFailureConstraint failure_constraint{database};
  const sphinx::UpdateProductRequest request{product_id, "updated", 2, 1};
  sphinx::MySqlProductStore store_before_trigger_drop{*options};

  try {
    (void)store_before_trigger_drop.update(request);
    FAIL() << "constraint violation before commit should fail";
  } catch (const sphinx::StoreError& error) {
    EXPECT_EQ(error.code(), sphinx::StoreErrorCode::Unexpected);
  }

  sphinx::StoreErrorCode second_error = sphinx::StoreErrorCode::Unexpected;
  bool second_call_threw = false;
  bool second_call_failed = false;
  std::thread second_worker{[&] {
    try {
      sphinx::MySqlThreadGuard worker;
      sphinx::MySqlProductStore store{*options};
      try {
        (void)store.update(request);
      } catch (const sphinx::StoreError& error) {
        second_error = error.code();
        second_call_threw = true;
      }
    } catch (...) {
      second_call_failed = true;
    }
  }};
  second_worker.join();
  EXPECT_FALSE(second_call_failed);
  EXPECT_TRUE(second_call_threw);
  EXPECT_EQ(second_error, sphinx::StoreErrorCode::Unexpected);
  failure_constraint.drop();

  sphinx::MySqlProductStore store_for_commit_unknown{*options};
  const int calls_before = commit_calls.load();
  inject_error_after_real_commit.store(true);
  try {
    (void)store_for_commit_unknown.update(request);
    FAIL() << "commit response failure should be ambiguous";
  } catch (const sphinx::StoreError& error) {
    EXPECT_EQ(error.code(), sphinx::StoreErrorCode::CommitUnknown);
  }
  EXPECT_EQ(commit_calls.load() - calls_before, 1);
  EXPECT_FALSE(inject_error_after_real_commit.load());

  const auto product = store_for_commit_unknown.find(product_id);
  ASSERT_TRUE(product.has_value());
  EXPECT_EQ(product->version, 2);
  EXPECT_EQ(product->name, request.name);
  EXPECT_EQ(product->price_cents, request.price_cents);
}
