// SPDX-License-Identifier: Apache-2.0
#include "command_executor.h"

#include <sphinx/string.h>

#include <utility>
namespace sphinx {
namespace {

// 执行存储类修改命令（Set、Add、Replace、Delete、Incr、Decr）
ExecutionResult execute_storage(Log& log, const Command& command) {
  switch (command.op) {
    // 1. 写入类命令（Set / Add / Replace）
    case Opcode::Set:
    case Opcode::Add:
    case Opcode::Replace: {
      // Add 要求键不存在；Replace 要求键必须已存在
      if (command.op != Opcode::Set) {
        const bool found = static_cast<bool>(log.find_value(command.key));
        if ((command.op == Opcode::Add && found) || (command.op == Opcode::Replace && !found)) {
          return {"NOT_STORED\r\n"};
        }
      }

      // 执行日志内存追加写入
      return log.append(command.key, command.blob, command.flags, command.expiration)
                 ? ExecutionResult{"STORED\r\n"}
                 : ExecutionResult{"SERVER_ERROR out of memory storing object\r\n"};
    }

    // 2. 删除命令（Delete）
    case Opcode::Delete:
      return {log.remove(command.key) ? "DELETED\r\n" : "NOT_FOUND\r\n"};

    // 3. 算术自增/自减命令（Incr / Decr）
    case Opcode::Incr:
    case Opcode::Decr: {
      const auto [status, val] = command.op == Opcode::Incr ? log.incr(command.key, command.delta)
                                                            : log.decr(command.key, command.delta);
      switch (status) {
        case ArithmeticStatus::Success:
          return {to_string(val) + "\r\n"};
        case ArithmeticStatus::NotFound:
          return {"NOT_FOUND\r\n"};
        case ArithmeticStatus::NonNumeric:
          return {"CLIENT_ERROR cannot increment or decrement non-numeric value\r\n"};
        case ArithmeticStatus::StorageFull:
          return {"SERVER_ERROR out of memory storing object\r\n"};
      }
      break;
    }

    case Opcode::Get:
    case Opcode::Version:
    case Opcode::Stats:
      break;
  }

  return {};
}

// 执行读取查询命令（Get / multi-get 分片）
ExecutionResult execute_get(Log& log, ServerStats& stats, const Command& command) {
  std::string response;

  // 查询键是否存在且未过期
  if (auto search = log.find_value(command.key)) {
    // 命中：原子更新统计指标，格式化 VALUE 响应行与正文
    stats.increment(ServerStats::Counter::GetHits);
    const auto& value = search.value();
    response.reserve(command.key.size() + value.blob.size() + 48);

    response += "VALUE ";
    response += command.key;
    response += ' ';
    response += to_string(value.flags);
    response += ' ';
    response += to_string(value.blob.size());
    response += "\r\n";
    response += value.blob;
    response += "\r\n";
  } else {
    // 未命中：原子递增未命中统计计数
    stats.increment(ServerStats::Counter::GetMisses);
  }

  // multi-get 的各个子分片无需附加独立的 END\r\n，由 Connection 聚合后统一追加
  if (command.multi_get) {
    return {std::move(response)};
  }

  // 单键 get 请求以 END\r\n 结尾
  response += "END\r\n";
  return {std::move(response)};
}

}  // namespace

// 命令执行统一入口：根据操作码将请求派发到对应的执行子逻辑
ExecutionResult execute_command(Log& log, ServerStats& stats, const Command& command) {
  switch (command.op) {
    case Opcode::Version:
      return {"VERSION 1.5.16\r\n"};

    case Opcode::Stats:
      return {stats.render()};

    case Opcode::Get:
      return execute_get(log, stats, command);

    case Opcode::Set:
    case Opcode::Add:
    case Opcode::Replace:
    case Opcode::Delete:
    case Opcode::Incr:
    case Opcode::Decr:
      return execute_storage(log, command);
  }

  return {};
}

}  // namespace sphinx
