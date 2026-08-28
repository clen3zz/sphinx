// SPDX-License-Identifier: Apache-2.0
#include "command_executor.h"

#include <sphinx/string.h>

#include <utility>
namespace sphinx::server {
namespace {
ExecutionResult execute_storage(sphinx::logmem::Log& log, const Command& command) {
  using sphinx::memcache::Opcode;
  switch (command.op) {
    case Opcode::Set:
    case Opcode::Add:
    case Opcode::Replace: {
      if (command.op != Opcode::Set) {
        const bool found = static_cast<bool>(log.find_value(command.key));
        if ((command.op == Opcode::Add && found) || (command.op == Opcode::Replace && !found)) {
          return {"NOT_STORED\r\n"};
        }
      }
      return log.append(command.key, command.blob, command.flags, command.expiration)
                 ? ExecutionResult{"STORED\r\n"}
                 : ExecutionResult{"SERVER_ERROR out of memory storing object\r\n"};
    }

    case Opcode::Delete:
      return {log.remove(command.key) ? "DELETED\r\n" : "NOT_FOUND\r\n"};

    case Opcode::Incr:
    case Opcode::Decr: {
      const auto arithmetic = command.op == Opcode::Incr ? log.incr(command.key, command.delta)
                                                         : log.decr(command.key, command.delta);
      switch (arithmetic.status) {
        case sphinx::logmem::ArithmeticStatus::Success:
          return {sphinx::to_string(arithmetic.value) + "\r\n"};
        case sphinx::logmem::ArithmeticStatus::NotFound:
          return {"NOT_FOUND\r\n"};
        case sphinx::logmem::ArithmeticStatus::NonNumeric:
          return {"CLIENT_ERROR cannot increment or decrement non-numeric value\r\n"};
        case sphinx::logmem::ArithmeticStatus::StorageFull:
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
ExecutionResult execute_get(sphinx::logmem::Log& log, sphinx::stats::ServerStats& stats,
                            const Command& command) {
  std::string response;
  auto search = log.find_value(command.key);
  if (search) {
    stats.increment(sphinx::stats::ServerStats::Counter::GetHits);
    const auto& value = search.value();
    response.reserve(command.key.size() + value.blob.size() + 48);
    response += "VALUE ";
    response += command.key;
    response += " ";
    response += sphinx::to_string(value.flags);
    response += " ";
    response += sphinx::to_string(value.blob.size());
    response += "\r\n";
    response += value.blob;
    response += "\r\n";
  } else {
    stats.increment(sphinx::stats::ServerStats::Counter::GetMisses);
  }
  if (command.multi_get) {
    return {std::move(response)};
  }
  response += "END\r\n";
  return {std::move(response)};
}
}  // namespace
ExecutionResult execute_command(sphinx::logmem::Log& log, sphinx::stats::ServerStats& stats,
                                const Command& command) {
  using sphinx::memcache::Opcode;
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
}  // namespace sphinx::server
