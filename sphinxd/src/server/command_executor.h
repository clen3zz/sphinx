// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/logmem.h>
#include <sphinx/stats.h>

#include <string>

#include "message.h"
namespace sphinx::server {

// 命令执行结果包装（包含待回传客户端的响应载荷）
struct ExecutionResult {
  std::string payload;
};

// 命令执行分发入口：在目标工作线程上直接访问所属的私有 Log 与全局 Stats
// Command 结构体中携带了路由与响应所需的元数据
ExecutionResult execute_command(sphinx::logmem::Log& log, sphinx::stats::ServerStats& stats,
                                const Command& command);

}  // namespace sphinx::server
