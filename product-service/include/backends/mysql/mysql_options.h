// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace sphinx {

struct MySqlOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user;
  std::string password;
  std::string database;
  std::uint32_t connect_timeout_seconds = 2;
  std::uint32_t read_timeout_seconds = 2;
  std::uint32_t write_timeout_seconds = 2;
};

void validate_mysql_options(const MySqlOptions& options);

}  // namespace sphinx
