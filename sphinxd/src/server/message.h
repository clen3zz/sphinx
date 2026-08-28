// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/protocol.h>
#include <sphinx/reactor.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
namespace sphinx::server {

// 命令自行持有数据，因此跨越反应器队列时无需借用解析器数据。
struct Command {
  uint64_t connection_id = 0;
  uint64_t sequence = 0;
  size_t source_thread = 0;
  sphinx::memcache::Opcode op = sphinx::memcache::Opcode::Version;
  std::string key;
  std::string blob;
  uint32_t flags = 0;
  uint64_t expiration = 0;
  uint64_t delta = 0;
  bool multi_get = false;
  uint32_t key_index = 0;
  Command(uint64_t id,  // NOLINT(bugprone-easily-swappable-parameters)：构造点使用明确的字段顺序
          uint64_t request, size_t thread, sphinx::memcache::Opcode opcode, std::string command_key)
      : connection_id{id},
        sequence{request},
        source_thread{thread},
        op{opcode},
        key{std::move(command_key)} {
  }
};
// 同一种响应类型同时表示完整回复和多键回复片段。
struct Response {
  uint64_t connection_id = 0;
  uint64_t sequence = 0;
  std::string payload;
  bool multi_get = false;
  uint32_t key_index = 0;
};
using MessagePayload = std::variant<Command, Response>;
// 在反应器边界明确限定合法的载荷类型。
struct Message : sphinx::reactor::Message {
  MessagePayload payload;

  template <typename Payload>
  explicit Message(Payload value) : payload{std::move(value)} {
  }
};
}  // namespace sphinx::server
