/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sphinx::memcache {

// Parsing a command header and receiving its value are deliberately separate
// operations.  The parser consumes one CRLF-terminated header at a time; a
// storage command's Body describes where the following value is expected.
struct StorageBody
{
  uint64_t size = 0;
  size_t offset = 0;

  // The wire frame consists of the value followed by its trailing CRLF.
  // Returning nullopt here is important: a uint64 length may not fit in a
  // size_t, and adding the CRLF must not wrap.
  std::optional<size_t> frame_size() const noexcept
  {
    constexpr auto max_size = std::numeric_limits<size_t>::max();
    if (size > static_cast<uint64_t>(max_size - 2)) {
      return std::nullopt;
    }
    return static_cast<size_t>(size) + 2;
  }

  bool available(std::string_view input) const noexcept
  {
    const auto frame = frame_size();
    return frame.has_value() && offset <= input.size() && *frame <= input.size() - offset;
  }

  bool has_valid_terminator(std::string_view input) const noexcept
  {
    if (!available(input)) {
      return false;
    }
    const auto value_size = static_cast<size_t>(size);
    return input[offset + value_size] == '\r' && input[offset + value_size + 1] == '\n';
  }

  // Return the value only when the complete frame, including the trailing
  // CRLF, is present and valid.  A null result means either that the receive
  // buffer needs more bytes or that the frame is malformed/too large.
  std::optional<std::string_view> view(std::string_view input) const noexcept
  {
    if (!has_valid_terminator(input)) {
      return std::nullopt;
    }
    return input.substr(offset, static_cast<size_t>(size));
  }
};

struct SetCommand
{
  std::string key;
  uint64_t flags = 0;
  uint64_t expiration = 0;
  StorageBody body;
};

struct AddCommand
{
  std::string key;
  uint64_t flags = 0;
  uint64_t expiration = 0;
  StorageBody body;
};

struct ReplaceCommand
{
  std::string key;
  uint64_t flags = 0;
  uint64_t expiration = 0;
  StorageBody body;
};

struct GetCommand
{
  std::vector<std::string> keys;
};

struct DeleteCommand
{
  std::string key;
};

struct IncrCommand
{
  std::string key;
  uint64_t delta = 0;
};

struct DecrCommand
{
  std::string key;
  uint64_t delta = 0;
};

struct VersionCommand
{
};

struct StatsCommand
{
};

// A parsed command owns all textual fields.  In particular, GetCommand keys
// do not point into the receive buffer, which the reactor may compact or
// release immediately after parse() returns.
using ParsedCommand = std::variant<SetCommand,
                                   AddCommand,
                                   ReplaceCommand,
                                   GetCommand,
                                   DeleteCommand,
                                   IncrCommand,
                                   DecrCommand,
                                   VersionCommand,
                                   StatsCommand>;

enum class ParseStatus
{
  Incomplete,
  Invalid,
  Parsed,
};

} // namespace sphinx::memcache
