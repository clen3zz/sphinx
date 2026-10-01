// SPDX-License-Identifier: Apache-2.0
#include <hiredis.h>
#include <sphinx/product/backends/redis/redis_product_cache.h>
#include <sys/time.h>

#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

struct RedisContextDeleter {
  void operator()(redisContext* context) const noexcept { redisFree(context); }
};

struct RedisReplyDeleter {
  void operator()(redisReply* reply) const noexcept { freeReplyObject(reply); }
};

using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

timeval to_timeval(std::chrono::milliseconds timeout) noexcept {
  const auto milliseconds = timeout.count();
  timeval result{};
  result.tv_sec = static_cast<time_t>(milliseconds / 1000);
  result.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
  return result;
}

struct RedisArgv {
  explicit RedisArgv(const std::vector<std::string_view>& arguments) {
    if (arguments.empty() ||
        arguments.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument{"invalid Redis command argument count"};
    }
    // hiredis 在 command/append 调用内复制字节；此处只借用参数，无需复制 payload。
    pointers.reserve(arguments.size());
    lengths.reserve(arguments.size());
    for (const auto argument : arguments) {
      pointers.push_back(argument.empty() ? "" : argument.data());
      lengths.push_back(argument.size());
    }
  }

  std::vector<const char*> pointers;
  std::vector<std::size_t> lengths;
};

}  // namespace

void validate_redis_options(const RedisOptions& options) {
  constexpr std::chrono::milliseconds max_timeout{10000};
  if (options.host.empty() || options.port == 0 || options.database > 15 ||
      options.connect_timeout.count() <= 0 || options.connect_timeout > max_timeout ||
      options.io_timeout.count() <= 0 || options.io_timeout > max_timeout ||
      (!options.username.empty() && options.password.empty())) {
    throw std::invalid_argument{"invalid Redis cache options"};
  }
}

class RedisProductCache::Impl final {
 public:
  explicit Impl(RedisOptions options)
      : _options{std::move(options)}, _owner_thread{std::this_thread::get_id()} {
    validate_redis_options(_options);
  }

  void check_owner() const {
    if (std::this_thread::get_id() != _owner_thread) {
      throw CacheError{"Redis cache used from a different thread"};
    }
  }

  RedisReplyPtr command(const std::vector<std::string_view>& arguments) {
    RedisArgv argv{arguments};
    ensure_connection();
    auto* raw_reply = static_cast<redisReply*>(
        redisCommandArgv(_context.get(), static_cast<int>(argv.pointers.size()),
                         argv.pointers.data(), argv.lengths.data()));
    if (raw_reply == nullptr) {
      reset_connection();
      throw CacheError{"Redis command failed"};
    }
    return RedisReplyPtr{raw_reply};
  }

  void append_command(const std::vector<std::string_view>& arguments) {
    RedisArgv argv{arguments};
    if (redisAppendCommandArgv(_context.get(), static_cast<int>(argv.pointers.size()),
                               argv.pointers.data(), argv.lengths.data()) != REDIS_OK) {
      reset_connection();
      throw CacheError{"Redis pipeline append failed"};
    }
  }

  RedisReplyPtr read_reply() {
    void* raw_reply = nullptr;
    if (redisGetReply(_context.get(), &raw_reply) != REDIS_OK || raw_reply == nullptr) {
      reset_connection();
      throw CacheError{"Redis pipeline reply failed"};
    }
    return RedisReplyPtr{static_cast<redisReply*>(raw_reply)};
  }

  std::optional<std::string> parse_value(const redisReply& reply) {
    if (reply.type == REDIS_REPLY_NIL) {
      return std::nullopt;
    }
    if (reply.type == REDIS_REPLY_STRING && (reply.str != nullptr || reply.len == 0)) {
      if (reply.len == 0) {
        return std::string{};
      }
      return std::string{reply.str, reply.len};
    }
    if (reply.type == REDIS_REPLY_ERROR) {
      throw CacheError{"Redis cache read was rejected"};
    }
    reset_connection();
    throw CacheError{"Redis cache read returned an unexpected response"};
  }

  void expect_ok(const redisReply& reply, const char* operation) {
    if (reply.type == REDIS_REPLY_ERROR) {
      throw CacheError{std::string{"Redis "} + operation + " was rejected"};
    }
    if (reply.type == REDIS_REPLY_STATUS && reply.str != nullptr && reply.len == 2 &&
        reply.str[0] == 'O' && reply.str[1] == 'K') {
      return;
    }
    reset_connection();
    throw CacheError{std::string{"Redis "} + operation + " returned an unexpected response"};
  }

  void expect_deleted(const redisReply& reply) {
    if (reply.type == REDIS_REPLY_ERROR) {
      throw CacheError{"Redis DEL was rejected"};
    }
    if (reply.type == REDIS_REPLY_INTEGER && (reply.integer == 0 || reply.integer == 1)) {
      return;
    }
    reset_connection();
    throw CacheError{"Redis DEL returned an unexpected response"};
  }

  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) {
    check_owner();
    validate_product_cache_keys(keys);
    if (keys.empty()) {
      return {};
    }

    std::vector<std::string_view> arguments;
    arguments.reserve(keys.size() + 1);
    arguments.emplace_back("MGET");
    for (const auto& key : keys) {
      arguments.emplace_back(key);
    }
    const RedisReplyPtr reply = command(arguments);
    if (reply->type == REDIS_REPLY_ERROR) {
      throw CacheError{"Redis MGET was rejected"};
    }
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != keys.size() ||
        reply->element == nullptr) {
      reset_connection();
      throw CacheError{"Redis MGET returned an unexpected response"};
    }

    std::vector<std::optional<std::string>> values;
    values.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
      if (reply->element[index] == nullptr) {
        reset_connection();
        throw CacheError{"Redis MGET returned an invalid element"};
      }
      values.push_back(parse_value(*reply->element[index]));
    }
    return values;
  }

  void put_many(const std::vector<CacheWriteEntry>& entries) {
    check_owner();
    validate_product_cache_writes(entries);
    if (entries.empty()) {
      return;
    }

    ensure_connection();
    bool command_rejected = false;
    try {
      for (const auto& entry : entries) {
        const std::string ttl = std::to_string(entry.ttl_seconds);
        append_command({"SET", entry.key, entry.value, "EX", ttl});
      }
      for (std::size_t index = 0; index < entries.size(); ++index) {
        const RedisReplyPtr reply = read_reply();
        if (reply->type == REDIS_REPLY_ERROR) {
          command_rejected = true;
        } else {
          expect_ok(*reply, "SET");
        }
      }
    } catch (...) {
      // 未读完的回复或尚未发送的命令不能留给下一次操作。
      reset_connection();
      throw;
    }
    if (command_rejected) {
      throw CacheError{"Redis pipeline SET was rejected"};
    }
  }

 private:
  void ensure_connection() {
    if (_context != nullptr) {
      return;
    }
    RedisContextPtr context{redisConnectWithTimeout(_options.host.c_str(),
                                                    static_cast<int>(_options.port),
                                                    to_timeval(_options.connect_timeout))};
    if (context == nullptr || context->err != 0) {
      throw CacheError{"Redis connection failed"};
    }
    if (redisSetTimeout(context.get(), to_timeval(_options.io_timeout)) != REDIS_OK) {
      throw CacheError{"Redis I/O timeout setup failed"};
    }
    _context = std::move(context);

    try {
      if (!_options.username.empty()) {
        expect_startup_ok({"AUTH", _options.username, _options.password}, "AUTH");
      } else if (!_options.password.empty()) {
        expect_startup_ok({"AUTH", _options.password}, "AUTH");
      }
      if (_options.database != 0) {
        const std::string database = std::to_string(_options.database);
        expect_startup_ok({"SELECT", database}, "SELECT");
      }
    } catch (...) {
      reset_connection();
      throw;
    }
  }

  void expect_startup_ok(const std::vector<std::string_view>& arguments, const char* operation) {
    RedisReplyPtr reply = command(arguments);
    expect_ok(*reply, operation);
  }

  void reset_connection() noexcept { _context.reset(); }

  RedisOptions _options;
  std::thread::id _owner_thread;
  RedisContextPtr _context;
};

RedisProductCache::RedisProductCache(RedisOptions options)
    : _impl{std::make_unique<Impl>(std::move(options))} {}

RedisProductCache::~RedisProductCache() = default;

std::optional<std::string> RedisProductCache::get(std::string_view key) {
  _impl->check_owner();
  if (!valid_product_cache_key(key)) {
    throw std::invalid_argument{"invalid product cache key"};
  }
  const RedisReplyPtr reply = _impl->command({"GET", key});
  return _impl->parse_value(*reply);
}

void RedisProductCache::put(std::string_view key, std::string_view value,
                            std::uint32_t ttl_seconds) {
  _impl->check_owner();
  if (!valid_product_cache_key(key)) {
    throw std::invalid_argument{"invalid product cache key"};
  }
  if (!valid_product_cache_ttl(ttl_seconds)) {
    throw std::invalid_argument{"cache TTL must be in 1..30 days"};
  }
  const std::string ttl = std::to_string(ttl_seconds);
  const RedisReplyPtr reply = _impl->command({"SET", key, value, "EX", ttl});
  _impl->expect_ok(*reply, "SET");
}

void RedisProductCache::erase(std::string_view key) {
  _impl->check_owner();
  if (!valid_product_cache_key(key)) {
    throw std::invalid_argument{"invalid product cache key"};
  }
  const RedisReplyPtr reply = _impl->command({"DEL", key});
  _impl->expect_deleted(*reply);
}

std::vector<std::optional<std::string>> RedisProductCache::get_many(
    const std::vector<std::string>& keys) {
  return _impl->get_many(keys);
}

void RedisProductCache::put_many(const std::vector<CacheWriteEntry>& entries) {
  _impl->put_many(entries);
}

}  // namespace sphinx
