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

// Owning commands can cross the reactor queue without borrowing parser data.
struct Command
{
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
  Command(uint64_t id,
          uint64_t request,
          size_t thread,
          sphinx::memcache::Opcode opcode,
          std::string command_key)
    : connection_id{id}
    , sequence{request}
    , source_thread{thread}
    , op{opcode}
    , key{std::move(command_key)}
  {
  }
};
// One response type covers complete replies and multi-key pieces.
struct Response
{
  uint64_t connection_id = 0;
  uint64_t sequence = 0;
  std::string payload;
  bool multi_get = false;
  uint32_t key_index = 0;
};
using MessagePayload = std::variant<Command, Response>;
// Keep legal payload types explicit at the reactor boundary.
struct Message : sphinx::reactor::Message
{
  MessagePayload payload;

  template<typename Payload>
  explicit Message(Payload value)
    : payload{std::move(value)}
  {
  }
};
} // namespace sphinx::server
