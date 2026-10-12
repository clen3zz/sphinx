// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace sphinx {

struct RedisOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;
  std::uint32_t database = 0;
  std::string username;
  std::string password;
  std::chrono::milliseconds connect_timeout{200};
  std::chrono::milliseconds io_timeout{200};
};

void validate_redis_options(const RedisOptions& options);

}  // namespace sphinx
