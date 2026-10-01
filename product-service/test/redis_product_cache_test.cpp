// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <hiredis.h>
#include <netinet/in.h>
#include <sphinx/redis_product_cache.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

using RedisTestCommand = std::vector<std::string>;
constexpr std::size_t max_request_bulk_size = std::size_t{1024} * 1024;

// 复用 hiredis 的 RESP 解析器；socket 分片和 pipeline 的剩余字节由 reader 保存。
std::optional<RedisTestCommand> read_command(int socket, redisReader& reader) {
  while (true) {
    void* raw_reply = nullptr;
    if (redisReaderGetReply(&reader, &raw_reply) != REDIS_OK) {
      throw std::runtime_error{"Redis test request contains invalid RESP"};
    }
    if (raw_reply != nullptr) {
      std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply{
          static_cast<redisReply*>(raw_reply), freeReplyObject};
      if (reply->type != REDIS_REPLY_ARRAY || reply->elements == 0 || reply->elements > 64) {
        throw std::runtime_error{"Redis test request has an invalid argument count"};
      }
      RedisTestCommand command;
      command.reserve(reply->elements);
      for (std::size_t index = 0; index < reply->elements; ++index) {
        const auto* argument = reply->element[index];
        if (argument == nullptr || argument->type != REDIS_REPLY_STRING ||
            argument->len > max_request_bulk_size) {
          throw std::runtime_error{"Redis test request contains an invalid bulk value"};
        }
        command.emplace_back(argument->len == 0 ? "" : argument->str, argument->len);
      }
      return command;
    }
    char bytes[4096]{};
    const auto count = recv(socket, bytes, sizeof(bytes), 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    if (redisReaderFeed(&reader, bytes, static_cast<std::size_t>(count)) != REDIS_OK) {
      throw std::runtime_error{"Redis test request parsing failed"};
    }
  }
}

bool write_all(int socket, std::string_view reply) {
  std::size_t written = 0;
  while (written < reply.size()) {
    const auto result = send(socket, reply.data() + written, reply.size() - written, MSG_NOSIGNAL);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

int make_listener(std::uint16_t* port) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    throw std::system_error{errno, std::generic_category(), "create Redis test socket"};
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 4) != 0) {
    const int error = errno;
    close(listener);
    throw std::system_error{error, std::generic_category(), "bind Redis test socket"};
  }
  socklen_t address_size = sizeof(address);
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
    const int error = errno;
    close(listener);
    throw std::system_error{error, std::generic_category(), "read Redis test port"};
  }
  *port = ntohs(address.sin_port);
  return listener;
}

class ScriptedRedisServer final {
 public:
  using Handler = std::function<std::string(const RedisTestCommand&)>;

  explicit ScriptedRedisServer(Handler handler) : _handler{std::move(handler)} {
    _listen_socket = make_listener(&_port);
    try {
      _thread = std::thread{[this] { serve(); }};
    } catch (...) {
      close(_listen_socket);
      throw;
    }
  }

  ~ScriptedRedisServer() {
    _stopping.store(true, std::memory_order_release);
    (void)shutdown(_listen_socket, SHUT_RDWR);
    const int wake_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (wake_socket >= 0) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(_port);
      const int connected =
          connect(wake_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      if (connected == 0) {
        (void)shutdown(wake_socket, SHUT_RDWR);
      }
      close(wake_socket);
    }
    if (_thread.joinable()) {
      _thread.join();
    }
    close(_listen_socket);
  }

  ScriptedRedisServer(const ScriptedRedisServer&) = delete;
  ScriptedRedisServer& operator=(const ScriptedRedisServer&) = delete;

  std::uint16_t port() const noexcept { return _port; }

  std::vector<RedisTestCommand> commands() const {
    std::lock_guard<std::mutex> lock{_mutex};
    return _commands;
  }

  std::string error() const {
    std::lock_guard<std::mutex> lock{_mutex};
    return _error;
  }

 private:
  void serve() noexcept {
    while (!_stopping.load(std::memory_order_acquire)) {
      const int client = accept(_listen_socket, nullptr, nullptr);
      if (client < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (_stopping.load(std::memory_order_acquire)) {
          return;
        }
        set_error("Redis test accept failed");
        return;
      }
      if (_stopping.load(std::memory_order_acquire)) {
        close(client);
        return;
      }
      timeval timeout{2, 0};
      (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      serve_client(client);
      close(client);
    }
  }

  void serve_client(int client) noexcept {
    try {
      std::unique_ptr<redisReader, decltype(&redisReaderFree)> reader{redisReaderCreate(),
                                                                      redisReaderFree};
      if (!reader) {
        throw std::runtime_error{"Redis test reader allocation failed"};
      }
      while (!_stopping.load(std::memory_order_acquire)) {
        const auto command = read_command(client, *reader);
        if (!command) {
          return;
        }
        {
          std::lock_guard<std::mutex> lock{_mutex};
          _commands.push_back(*command);
        }
        const std::string reply = _handler(*command);
        if (reply.empty() || !write_all(client, reply)) {
          return;
        }
      }
    } catch (const std::exception& error) {
      set_error(error.what());
    }
  }

  void set_error(std::string message) noexcept {
    std::lock_guard<std::mutex> lock{_mutex};
    _error = std::move(message);
  }

  std::uint16_t _port = 0;
  int _listen_socket = -1;
  Handler _handler;
  std::atomic<bool> _stopping{false};
  mutable std::mutex _mutex;
  std::vector<RedisTestCommand> _commands;
  std::string _error;
  std::thread _thread;
};

RedisOptions test_options(std::uint16_t port) {
  RedisOptions options;
  options.port = port;
  options.connect_timeout = std::chrono::milliseconds{500};
  options.io_timeout = std::chrono::milliseconds{500};
  return options;
}

std::string bulk_reply(std::string_view value) {
  return "$" + std::to_string(value.size()) + "\r\n" + std::string{value} + "\r\n";
}

std::string array_reply(const std::vector<std::optional<std::string>>& values) {
  std::string reply = "*" + std::to_string(values.size()) + "\r\n";
  for (const auto& value : values) {
    if (value) {
      reply += bulk_reply(value.value_or(""));
    } else {
      reply += "$-1\r\n";
    }
  }
  return reply;
}

TEST(RedisProductCacheTest, UsesBinarySafeGetSetAndDeleteCommands) {
  const std::string key{"product:v2:42"};
  const std::string value{"A\0B\r\nZ", 6};
  ScriptedRedisServer server{[&](const RedisTestCommand& command) {
    if (command[0] == "GET") {
      return bulk_reply(value);
    }
    if (command[0] == "SET") {
      return std::string{"+OK\r\n"};
    }
    return std::string{":0\r\n"};
  }};
  RedisProductCache cache{test_options(server.port())};

  EXPECT_TRUE(server.commands().empty());
  const auto result = cache.get(key);
  const std::string actual = result.value_or(std::string{});
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(actual, value);
  cache.put(key, value, 9);
  cache.erase(key);

  const auto commands = server.commands();
  ASSERT_EQ(commands.size(), 3U);
  EXPECT_EQ(commands[0], (RedisTestCommand{"GET", key}));
  EXPECT_EQ(commands[1], (RedisTestCommand{"SET", key, value, "EX", "9"}));
  EXPECT_EQ(commands[2], (RedisTestCommand{"DEL", key}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, AuthenticatesAndSelectsDatabaseWithSeparateArguments) {
  ScriptedRedisServer server{[](const RedisTestCommand& command) {
    if (command[0] == "GET") {
      return std::string{"$-1\r\n"};
    }
    return std::string{"+OK\r\n"};
  }};
  RedisOptions options = test_options(server.port());
  options.database = 3;
  options.username = "reader";
  options.password = "line\r\nINJECT";
  RedisProductCache cache{std::move(options)};

  EXPECT_FALSE(cache.get("product:v2:1"));
  EXPECT_EQ(server.commands(),
            (std::vector<RedisTestCommand>{
                {"AUTH", "reader", "line\r\nINJECT"}, {"SELECT", "3"}, {"GET", "product:v2:1"}}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, ResetsMalformedRepliesBeforeTheNextOperation) {
  std::atomic<unsigned int> get_count{0};
  ScriptedRedisServer server{[&](const RedisTestCommand&) {
    if (get_count.fetch_add(1, std::memory_order_relaxed) == 0) {
      return std::string{":7\r\n"};
    }
    return std::string{"$-1\r\n"};
  }};
  RedisProductCache cache{test_options(server.port())};

  EXPECT_THROW(cache.get("product:v2:1"), CacheError);
  EXPECT_FALSE(cache.get("product:v2:1"));
  EXPECT_EQ(server.commands().size(), 2U);
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, MGetPreservesOrderDuplicatesAndBinaryValues) {
  const std::string binary_value{"A\0B", 3};
  ScriptedRedisServer server{[&](const RedisTestCommand&) {
    return array_reply({std::string{"value"}, std::nullopt, std::string{"value"}, binary_value});
  }};
  RedisProductCache cache{test_options(server.port())};
  const std::vector<std::string> keys{"first", "missing", "first", "binary"};

  const auto values = cache.get_many(keys);

  ASSERT_EQ(values.size(), keys.size());
  EXPECT_EQ(values[0].value_or(""), "value");
  EXPECT_FALSE(values[1]);
  EXPECT_EQ(values[2].value_or(""), "value");
  EXPECT_EQ(values[3].value_or(""), binary_value);
  EXPECT_EQ(server.commands(),
            (std::vector<RedisTestCommand>{{"MGET", "first", "missing", "first", "binary"}}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, PutsBatchWithPipelinedSetCommandsAndExpiration) {
  ScriptedRedisServer server{[](const RedisTestCommand&) { return std::string{"+OK\r\n"}; }};
  RedisProductCache cache{test_options(server.port())};
  const std::vector<CacheWriteEntry> entries{
      {"first", "one", 10}, {"second", std::string{"A\0B", 3}, 20}, {"third", "three", 30}};

  cache.put_many(entries);

  EXPECT_EQ(server.commands(),
            (std::vector<RedisTestCommand>{{"SET", "first", "one", "EX", "10"},
                                           {"SET", "second", std::string{"A\0B", 3}, "EX", "20"},
                                           {"SET", "third", "three", "EX", "30"}}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, DrainsPipelineRepliesAfterServerCommandError) {
  ScriptedRedisServer server{[](const RedisTestCommand& command) {
    if (command[0] == "SET" && command[1] == "second") {
      return std::string{"-ERR simulated rejection\r\n"};
    }
    if (command[0] == "GET") {
      return std::string{"$-1\r\n"};
    }
    return std::string{"+OK\r\n"};
  }};
  RedisProductCache cache{test_options(server.port())};
  const std::vector<CacheWriteEntry> entries{
      {"first", "one", 10}, {"second", "two", 20}, {"third", "three", 30}};

  EXPECT_THROW(cache.put_many(entries), CacheError);
  EXPECT_FALSE(cache.get("after-pipeline-error"));

  EXPECT_EQ(server.commands().size(), 4U);
  EXPECT_EQ(server.commands()[2][1], "third");
  EXPECT_EQ(server.commands()[3][0], "GET");
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, DoesNotReplayPipelineAfterMidBatchDisconnect) {
  ScriptedRedisServer server{[](const RedisTestCommand& command) {
    if (command[0] == "GET") {
      return bulk_reply("recovered");
    }
    return command[1] == "second" ? std::string{} : std::string{"+OK\r\n"};
  }};
  RedisProductCache cache{test_options(server.port())};

  EXPECT_THROW(
      cache.put_many({{"first", "one", 10}, {"second", "two", 10}, {"third", "three", 10}}),
      CacheError);
  EXPECT_EQ(cache.get("after-disconnect").value_or(""), "recovered");
  EXPECT_EQ(server.commands(), (std::vector<RedisTestCommand>{{"SET", "first", "one", "EX", "10"},
                                                              {"SET", "second", "two", "EX", "10"},
                                                              {"GET", "after-disconnect"}}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, ResetsTimedOutGetAndReconnectsWithoutReplaying) {
  std::atomic<unsigned int> gets{0};
  ScriptedRedisServer server{[&](const RedisTestCommand&) {
    if (gets.fetch_add(1, std::memory_order_relaxed) == 0) {
      // 保持连接开放但不发送完整 bulk，迫使客户端在真实 socket 读取中超时。
      return std::string{"$5\r\nab"};
    }
    return bulk_reply("fresh");
  }};
  auto options = test_options(server.port());
  options.io_timeout = std::chrono::milliseconds{50};
  RedisProductCache cache{options};

  EXPECT_THROW(cache.get("stalled"), CacheError);
  EXPECT_EQ(cache.get("after-timeout").value_or(""), "fresh");
  EXPECT_EQ(server.commands(),
            (std::vector<RedisTestCommand>{{"GET", "stalled"}, {"GET", "after-timeout"}}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, ResetsTimedOutPipelineBeforeTheNextOperation) {
  std::mutex mutex;
  std::condition_variable ready;
  bool release_server = false;
  ScriptedRedisServer server{[&](const RedisTestCommand& command) {
    if (command[0] == "GET") {
      return bulk_reply("recovered");
    }
    if (command[1] == "second") {
      std::unique_lock lock{mutex};
      ready.wait_for(lock, std::chrono::seconds{2}, [&] { return release_server; });
      return std::string{};
    }
    return std::string{"+OK\r\n"};
  }};
  auto options = test_options(server.port());
  options.io_timeout = std::chrono::milliseconds{50};
  RedisProductCache cache{options};

  EXPECT_THROW(cache.put_many({{"first", "one", 10}, {"second", "two", 10}}), CacheError);
  {
    std::lock_guard lock{mutex};
    release_server = true;
  }
  ready.notify_all();
  EXPECT_EQ(cache.get("after-pipeline-timeout").value_or(""), "recovered");
  EXPECT_EQ(server.commands(), (std::vector<RedisTestCommand>{{"SET", "first", "one", "EX", "10"},
                                                              {"SET", "second", "two", "EX", "10"},
                                                              {"GET", "after-pipeline-timeout"}}));
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, ValidatesWholeBatchBeforeConnecting) {
  ScriptedRedisServer server{[](const RedisTestCommand&) { return std::string{"+OK\r\n"}; }};
  RedisProductCache cache{test_options(server.port())};

  EXPECT_TRUE(cache.get_many({}).empty());
  cache.put_many({});
  EXPECT_THROW(cache.get_many({"valid", "bad key"}), std::invalid_argument);
  EXPECT_THROW(cache.put_many({{"valid", "value", 10}, {"later", "value", 0}}),
               std::invalid_argument);
  const std::vector<std::string> oversized_keys(max_product_batch_size + 1, "valid");
  EXPECT_THROW(cache.get_many(oversized_keys), std::invalid_argument);
  std::vector<CacheWriteEntry> oversized_entries(max_product_batch_size + 1,
                                                 {"valid", "value", 10});
  EXPECT_THROW(cache.put_many(oversized_entries), std::invalid_argument);
  EXPECT_TRUE(server.commands().empty());
}

TEST(RedisProductCacheTest, ResetsMalformedMGetRepliesBeforeTheNextOperation) {
  std::atomic<unsigned int> request_count{0};
  ScriptedRedisServer server{[&](const RedisTestCommand&) {
    const unsigned int index = request_count.fetch_add(1, std::memory_order_relaxed);
    if (index == 0) {
      return std::string{"*0\r\n"};
    }
    if (index == 1) {
      return std::string{"*1\r\n:3\r\n"};
    }
    return std::string{"*1\r\n$-1\r\n"};
  }};
  RedisProductCache cache{test_options(server.port())};

  EXPECT_THROW(cache.get_many({"first"}), CacheError);
  EXPECT_THROW(cache.get_many({"second"}), CacheError);
  EXPECT_FALSE(cache.get_many({"third"})[0]);
  EXPECT_EQ(server.commands().size(), 3U);
  EXPECT_TRUE(server.error().empty());
}

TEST(RedisProductCacheTest, RejectsInvalidOptionsKeysAndWrongThreadUse) {
  RedisOptions invalid_options;
  invalid_options.port = 0;
  EXPECT_THROW(RedisProductCache cache{invalid_options}, std::invalid_argument);

  ScriptedRedisServer server{[](const RedisTestCommand&) { return std::string{"$-1\r\n"}; }};
  RedisProductCache cache{test_options(server.port())};
  EXPECT_THROW(cache.get("bad key"), std::invalid_argument);
  EXPECT_THROW(cache.put("product:v2:1", "value", 0), std::invalid_argument);

  std::atomic<bool> rejected{false};
  std::thread worker{[&] {
    try {
      (void)cache.get("product:v2:1");
    } catch (const CacheError&) {
      rejected.store(true, std::memory_order_relaxed);
    }
  }};
  worker.join();
  EXPECT_TRUE(rejected.load(std::memory_order_relaxed));
  EXPECT_TRUE(server.commands().empty());
  EXPECT_TRUE(server.error().empty());
}

}  // namespace
}  // namespace sphinx
