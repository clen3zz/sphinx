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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sphinx/protocol_types.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

%%{

machine memcache_protocol;

access _fsm_;

action key_start {
    key_start_ = p;
}

action key_end {
    keys_.emplace_back(key_start_, static_cast<size_t>(p - key_start_));
}

crlf = "\r\n";

key = [^ \t\r\n]+ >key_start %key_end;

number = digit+ >{ number_ = 0; number_token_overflow_ = false; } ${
    if (!number_token_overflow_) {
        auto digit_value = uint64_t(fc - '0');
        if (number_ > (std::numeric_limits<uint64_t>::max() - digit_value) / 10) {
            number_token_overflow_ = true;
            number_overflow_ = true;
        } else {
            number_ = number_ * 10 + digit_value;
        }
    }
};

flags = number %{ flags_ = number_; };

exptime = number %{ expiration_ = number_; };

bytes = number %{ body_size_ = number_; };

set = "set" space key space flags space exptime space bytes space? crlf @{ opcode_ = Opcode::Set; };

add = "add" space key space flags space exptime space bytes space? crlf @{ opcode_ = Opcode::Add; };

replace = "replace" space key space flags space exptime space bytes space? crlf @{ opcode_ = Opcode::Replace; };

get = "get" space key (space key)* crlf @{ opcode_ = Opcode::Get; };

delete = "delete" space key crlf @{ opcode_ = Opcode::Delete; };

delta = number %{ delta_ = number_; };

incr = "incr" space key space delta crlf @{ opcode_ = Opcode::Incr; };

decr = "decr" space key space delta crlf @{ opcode_ = Opcode::Decr; };

version = "version" crlf @{ opcode_ = Opcode::Version; };

stats = "stats" crlf @{ opcode_ = Opcode::Stats; };

main := (set | add | replace | get | delete | incr | decr | version | stats);

}%%

namespace sphinx::memcache {

%% write data nofinal noprefix;

enum class Opcode
{
  Set,
  Add,
  Replace,
  Get,
  Delete,
  Incr,
  Decr,
  Version,
  Stats,
};

class Parser
{
  int _fsm_cs;
  std::optional<ParsedCommand> _command;
  ParseStatus _status = ParseStatus::Incomplete;
  std::optional<Opcode> opcode_;
  const char* key_start_ = nullptr;
  std::vector<std::string> keys_;
  uint64_t number_ = 0;
  bool number_overflow_ = false;
  bool number_token_overflow_ = false;
  uint64_t flags_ = 0;
  uint64_t expiration_ = 0;
  uint64_t body_size_ = 0;
  uint64_t delta_ = 0;

public:
  const std::optional<ParsedCommand>& command() const noexcept
  {
    return _command;
  }

  ParseStatus status() const noexcept
  {
    return _status;
  }

  bool number_overflow() const noexcept
  {
    return number_overflow_;
  }

  Parser()
  {
    %% write init;
  }

  size_t parse(std::string_view msg)
  {
    _fsm_cs = start;
    _command.reset();
    _status = ParseStatus::Incomplete;
    opcode_.reset();
    key_start_ = nullptr;
    keys_.clear();
    number_ = 0;
    number_overflow_ = false;
    number_token_overflow_ = false;
    flags_ = 0;
    expiration_ = 0;
    body_size_ = 0;
    delta_ = 0;
    if (msg.empty()) {
      return 0;
    }
    auto* start = msg.data();
    auto header_end = msg.find("\r\n");
    if (header_end == std::string_view::npos) {
      // A command header is not complete until CRLF arrives.  Leaving the
      // command unset tells the reactor to retain the receive buffer.
      return 0;
    }
    auto* parse_end = start + header_end + 2;
    parse(start, parse_end);
    // The parser is deliberately bounded by the first CRLF, so even a
    // complete invalid line consumes exactly that line and cannot eat the
    // following pipelined command.
    const auto header_size = header_end + 2;
    if (opcode_) {
      build_command(header_size);
      _status = ParseStatus::Parsed;
    } else {
      _status = ParseStatus::Invalid;
    }
    return header_size;
  }

private:
  void build_command(size_t header_size)
  {
    const auto key_value = keys_.empty()
                             ? std::string_view{}
                             : std::string_view{keys_.front().data(), keys_.front().size()};
    const StorageBody body{body_size_, header_size};
    switch (opcode_.value()) {
      case Opcode::Set:
        _command = ParsedCommand{SetCommand{std::string{key_value}, flags_, expiration_, body}};
        break;
      case Opcode::Add:
        _command = ParsedCommand{AddCommand{std::string{key_value}, flags_, expiration_, body}};
        break;
      case Opcode::Replace:
        _command = ParsedCommand{ReplaceCommand{std::string{key_value}, flags_, expiration_, body}};
        break;
      case Opcode::Get:
        _command = ParsedCommand{GetCommand{std::move(keys_)}};
        break;
      case Opcode::Delete:
        _command = ParsedCommand{DeleteCommand{std::string{key_value}}};
        break;
      case Opcode::Incr:
        _command = ParsedCommand{IncrCommand{std::string{key_value}, delta_}};
        break;
      case Opcode::Decr:
        _command = ParsedCommand{DecrCommand{std::string{key_value}, delta_}};
        break;
      case Opcode::Version:
        _command = ParsedCommand{VersionCommand{}};
        break;
      case Opcode::Stats:
        _command = ParsedCommand{StatsCommand{}};
        break;
    }
    keys_.clear();
  }

  const char* parse(const char *p, const char *pe)
  {
    %% write exec;
    return p;
  }
};

}
