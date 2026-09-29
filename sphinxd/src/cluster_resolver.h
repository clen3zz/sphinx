// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <netdb.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace sphinx {

using AddressList = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;

// DNS 查询可能阻塞；调用方在统一的连接截止时间内等待结果。
AddressList resolve_with_deadline(const std::string& host, const std::string& port,
                                  std::chrono::steady_clock::time_point deadline,
                                  std::string_view target);

}  // namespace sphinx
