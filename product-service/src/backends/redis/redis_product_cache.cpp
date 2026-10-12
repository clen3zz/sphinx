// SPDX-License-Identifier: Apache-2.0
#include <hiredis.h>
#include <backends/redis/redis_product_cache.h>
#include <sys/time.h>

#include <cassert>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

timeval to_timeval(std::chrono::milliseconds timeout) noexcept {
  const auto milliseconds = timeout.count();
  timeval result{};
  result.tv_sec = static_cast<time_t>(milliseconds / 1000);
  result.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
  return result;
}

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

void RedisProductCache::ContextDeleter::operator()(redisContext* context) const noexcept {
  redisFree(context);
}
void RedisProductCache::ReplyDeleter::operator()(redisReply* reply) const noexcept {
  freeReplyObject(reply);
}

void RedisProductCache::check_owner() const {
  if (std::this_thread::get_id() != _owner_thread) {
    throw CacheError{"Redis cache used from a different thread"};
  }
}

RedisProductCache::ReplyPtr RedisProductCache::command(
    const std::vector<std::string_view>& arguments) {
  ensure_connection();
  append_command(arguments);
  return read_reply();
}

void RedisProductCache::append_command(const std::vector<std::string_view>& arguments) {
  assert(!arguments.empty() && arguments.size() <= max_product_batch_size + 1);
  std::vector<const char*> pointers;
  std::vector<std::size_t> lengths;
  pointers.reserve(arguments.size());
  lengths.reserve(arguments.size());
  // hiredis 在 append 内复制字节；这里借用参数，无需复制 payload。
  for (const auto argument : arguments) {
    pointers.push_back(argument.empty() ? "" : argument.data());
    lengths.push_back(argument.size());
  }
  if (redisAppendCommandArgv(_context.get(), static_cast<int>(pointers.size()), pointers.data(),
                             lengths.data()) != REDIS_OK) {
    _context.reset();
    throw CacheError{"Redis pipeline append failed"};
  }
}

RedisProductCache::ReplyPtr RedisProductCache::read_reply() {
  void* raw_reply = nullptr;
  if (redisGetReply(_context.get(), &raw_reply) != REDIS_OK || raw_reply == nullptr) {
    _context.reset();
    throw CacheError{"Redis pipeline reply failed"};
  }
  return ReplyPtr{static_cast<redisReply*>(raw_reply)};
}

void RedisProductCache::expect_reply(const redisReply& reply, bool valid,
                                     std::string_view operation) {
  if (reply.type == REDIS_REPLY_ERROR) {
    throw CacheError{"Redis " + std::string{operation} + " was rejected"};
  }
  if (!valid) {
    _context.reset();
    throw CacheError{"Redis " + std::string{operation} + " returned an unexpected response"};
  }
}

std::optional<std::string> RedisProductCache::parse_value(const redisReply& reply) {
  if (reply.type == REDIS_REPLY_NIL) {
    return std::nullopt;
  }
  expect_reply(reply, reply.type == REDIS_REPLY_STRING && (reply.str != nullptr || reply.len == 0),
               "GET");
  return reply.len != 0 ? std::string{reply.str, reply.len} : std::string{};
}

void RedisProductCache::expect_ok(const redisReply& reply, std::string_view operation) {
  expect_reply(reply,
               reply.type == REDIS_REPLY_STATUS && reply.str != nullptr &&
                   std::string_view{reply.str, reply.len} == "OK",
               operation);
}

std::vector<std::optional<std::string>> RedisProductCache::get_many(
    const std::vector<std::string>& keys) {
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
  const ReplyPtr reply = command(arguments);
  expect_reply(*reply,
               reply->type == REDIS_REPLY_ARRAY && reply->elements == keys.size() && reply->element,
               "MGET");

  std::vector<std::optional<std::string>> values;
  values.reserve(keys.size());
  for (std::size_t index = 0; index < keys.size(); ++index) {
    if (reply->element[index] == nullptr) {
      _context.reset();
      throw CacheError{"Redis MGET returned an invalid element"};
    }
    values.push_back(parse_value(*reply->element[index]));
  }
  return values;
}

void RedisProductCache::put_many(const std::vector<CacheWriteEntry>& entries) {
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
      const ReplyPtr reply = read_reply();
      if (reply->type == REDIS_REPLY_ERROR) {
        command_rejected = true;
      } else {
        expect_ok(*reply, "SET");
      }
    }
  } catch (...) {
    // 未读完的回复或尚未发送的命令不能留给下一次操作。
    _context.reset();
    throw;
  }
  if (command_rejected) {
    throw CacheError{"Redis pipeline SET was rejected"};
  }
}

void RedisProductCache::ensure_connection() {
  if (_context != nullptr) {
    return;
  }
  ContextPtr context{redisConnectWithTimeout(_options.host.c_str(), static_cast<int>(_options.port),
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
    _context.reset();
    throw;
  }
}

void RedisProductCache::expect_startup_ok(const std::vector<std::string_view>& arguments,
                                          const char* operation) {
  ReplyPtr reply = command(arguments);
  expect_ok(*reply, operation);
}

RedisProductCache::RedisProductCache(RedisOptions options) : _options{std::move(options)} {
  validate_redis_options(_options);
}

RedisProductCache::~RedisProductCache() = default;

std::optional<std::string> RedisProductCache::get(std::string_view key) {
  check_owner();
  if (!valid_product_cache_key(key)) {
    throw std::invalid_argument{"invalid product cache key"};
  }
  const ReplyPtr reply = command({"GET", key});
  return parse_value(*reply);
}

void RedisProductCache::put(std::string_view key, std::string_view value,
                            std::uint32_t ttl_seconds) {
  check_owner();
  if (!valid_product_cache_key(key)) {
    throw std::invalid_argument{"invalid product cache key"};
  }
  if (!valid_product_cache_ttl(ttl_seconds)) {
    throw std::invalid_argument{"cache TTL must be in 1..30 days"};
  }
  const std::string ttl = std::to_string(ttl_seconds);
  const ReplyPtr reply = command({"SET", key, value, "EX", ttl});
  expect_ok(*reply, "SET");
}

void RedisProductCache::erase(std::string_view key) {
  check_owner();
  if (!valid_product_cache_key(key)) {
    throw std::invalid_argument{"invalid product cache key"};
  }
  const ReplyPtr reply = command({"DEL", key});
  expect_reply(*reply,
               reply->type == REDIS_REPLY_INTEGER && (reply->integer == 0 || reply->integer == 1),
               "DEL");
}

}  // namespace sphinx
