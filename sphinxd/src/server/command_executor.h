// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "message.h"

#include <sphinx/logmem.h>
#include <sphinx/stats.h>

#include <string>
namespace sphinx::server {

struct ExecutionResult
{
  std::string payload;
};
// 在所属工作线程上执行；Command 携带响应所需的元数据。
ExecutionResult
execute_command(sphinx::logmem::Log& log,
                sphinx::stats::ServerStats& stats,
                const Command& command);
} // namespace sphinx::server
