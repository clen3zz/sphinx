// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sphinx/redis_product_cache.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <functional>
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

bool read_byte(int socket, char* value) {
  while (true) {
    const auto result = recv(socket, value, 1, 0);
    if (result == 1) {
      return true;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

std::optional<std::string> read_line(int socket) {
  std::string line;
  while (line.size() <= 1024) {
    char value = 0;
    if (!read_byte(socket, &value)) {
      return std::nullopt;
    }
    line.push_back(value);
    if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) {
      line.resize(line.size() - 2);
      return line;
    }
  }
  throw std::runtime_error{"Redis test request line is too long"};
}

std::size_t parse_size(std::string_view text) {
  std::size_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::runtime_error{"Redis test request contains an invalid length"};
  }
  return result;
}

std::optional<RedisTestCommand> read_command(int socket) {
  const auto header = read_line(socket);
  if (!header) {
    return std::nullopt;
  }
  if (header->empty() || (*header)[0] != '*') {
    throw std::runtime_error{"Redis test request is not an array"};
  }
  const std::size_t count = parse_size(std::string_view{*header}.substr(1));
  if (count == 0 || count > 16) {
    throw std::runtime_error{"Redis test request has an invalid argument count"};
  }

  RedisTestCommand command;
  command.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto bulk_header = read_line(socket);
    if (!bulk_header || bulk_header->empty() || (*bulk_header)[0] != '$') {
      throw std::runtime_error{"Redis test request contains an invalid bulk value"};
    }
    const std::size_t size = parse_size(std::string_view{*bulk_header}.substr(1));
    if (size > max_request_bulk_size) {
      throw std::runtime_error{"Redis test request bulk value is too large"};
    }
    std::string value(size, '\0');
    for (std::size_t position = 0; position < size; ++position) {
      if (!read_byte(socket, &value[position])) {
        throw std::runtime_error{"Redis test request ended in a bulk value"};
      }
    }
    char suffix[2]{};
    if (!read_byte(socket, &suffix[0]) || !read_byte(socket, &suffix[1]) || suffix[0] != '\r' ||
        suffix[1] != '\n') {
      throw std::runtime_error{"Redis test request has an invalid bulk terminator"};
    }
    command.push_back(std::move(value));
  }
  return command;
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
      while (!_stopping.load(std::memory_order_acquire)) {
        const auto command = read_command(client);
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
