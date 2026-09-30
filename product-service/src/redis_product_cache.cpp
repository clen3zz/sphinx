// SPDX-License-Identifier: Apache-2.0
#include <hiredis.h>
#include <sphinx/redis_product_cache.h>
#include <sys/time.h>

#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

struct RedisContextDeleter {
  void operator()(redisContext* context) const noexcept {
    if (context != nullptr) {
      redisFree(context);
    }
  }
};

struct RedisReplyDeleter {
  void operator()(redisReply* reply) const noexcept {
    if (reply != nullptr) {
      freeReplyObject(reply);
    }
  }
};

using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

RedisOptions checked_options(RedisOptions options) {
  constexpr std::uint32_t max_database = 15;
  constexpr std::chrono::milliseconds max_timeout{10000};
  if (options.host.empty() || options.port == 0 || options.database > max_database ||
      options.connect_timeout.count() <= 0 || options.connect_timeout > max_timeout ||
      options.io_timeout.count() <= 0 || options.io_timeout > max_timeout ||
      (!options.username.empty() && options.password.empty())) {
    throw std::invalid_argument{"invalid Redis cache options"};
  }
  return options;
}

timeval to_timeval(std::chrono::milliseconds timeout) noexcept {
  const auto milliseconds = timeout.count();
  timeval result{};
  result.tv_sec = static_cast<time_t>(milliseconds / 1000);
  result.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
  return result;
}

struct RedisArgv {
  explicit RedisArgv(const std::vector<std::string_view>& arguments) {
    values.reserve(arguments.size());
    for (const std::string_view argument : arguments) {
      if (argument.empty()) {
        values.emplace_back();
      } else {
        values.emplace_back(argument.data(), argument.size());
      }
    }

    pointers.reserve(values.size());
    lengths.reserve(values.size());
    for (const std::string& value : values) {
      pointers.push_back(value.data());
      lengths.push_back(value.size());
    }
  }

  std::vector<std::string> values;
  std::vector<const char*> pointers;
  std::vector<std::size_t> lengths;
};

}  // namespace

class RedisProductCache::Impl final {
 public:
  explicit Impl(RedisOptions options)
      : _options{std::move(options)}, _owner_thread{std::this_thread::get_id()} {}

  void check_owner() const {
    if (std::this_thread::get_id() != _owner_thread) {
      throw CacheError{"Redis cache used from a different thread"};
    }
  }

  RedisReplyPtr command(const std::vector<std::string_view>& arguments) {
    check_owner();
    if (arguments.empty() ||
        arguments.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument{"invalid Redis command argument count"};
    }
    ensure_connection();

    RedisArgv argv{arguments};
    auto* raw_reply = static_cast<redisReply*>(
        redisCommandArgv(_context.get(), static_cast<int>(argv.pointers.size()),
                         argv.pointers.data(), argv.lengths.data()));
    if (raw_reply == nullptr) {
      reset_connection();
      throw CacheError{"Redis command failed"};
    }
    return RedisReplyPtr{raw_reply};
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
      throw CacheError{"Redis GET was rejected"};
    }
    reset_connection();
    throw CacheError{"Redis GET returned an unexpected response"};
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

  bool expect_deleted(const redisReply& reply) {
    if (reply.type == REDIS_REPLY_ERROR) {
      throw CacheError{"Redis DEL was rejected"};
    }
    if (reply.type == REDIS_REPLY_INTEGER && (reply.integer == 0 || reply.integer == 1)) {
      return true;
    }
    reset_connection();
    throw CacheError{"Redis DEL returned an unexpected response"};
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
        (void)expect_startup_ok({"AUTH", _options.username, _options.password}, "AUTH");
      } else if (!_options.password.empty()) {
        (void)expect_startup_ok({"AUTH", _options.password}, "AUTH");
      }
      if (_options.database != 0) {
        const std::string database = std::to_string(_options.database);
        (void)expect_startup_ok({"SELECT", database}, "SELECT");
      }
    } catch (...) {
      reset_connection();
      throw;
    }
  }

  RedisReplyPtr expect_startup_ok(const std::vector<std::string_view>& arguments,
                                  const char* operation) {
    RedisReplyPtr reply = command(arguments);
    expect_ok(*reply, operation);
    return reply;
  }

  void reset_connection() noexcept { _context.reset(); }

  RedisOptions _options;
  std::thread::id _owner_thread;
  RedisContextPtr _context;
};

RedisProductCache::RedisProductCache(RedisOptions options)
    : _impl{std::make_unique<Impl>(checked_options(std::move(options)))} {}

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
  (void)_impl->expect_deleted(*reply);
}

}  // namespace sphinx
