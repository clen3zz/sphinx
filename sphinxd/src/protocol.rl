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
#include <string>
#include <string_view>
#include <vector>

%%{

machine memcache_protocol;

access _fsm_;

action key_start {
    _key_start = p;
}

action key_end {
    _key_end = p;
    _owned_keys.emplace_back(_key_start, static_cast<size_t>(_key_end - _key_start));
}

action blob_start {
    _blob_start = p + 1;
}

crlf = "\r\n";

key = [^ \t\r\n]+ >key_start %key_end;

number = digit+ >{ _number = 0; _current_number_overflow = false; } ${
    if (!_current_number_overflow) {
        auto digit_value = uint64_t(fc - '0');
        if (_number > (std::numeric_limits<uint64_t>::max() - digit_value) / 10) {
            _current_number_overflow = true;
            _number_overflow = true;
        } else {
            _number = _number * 10 + digit_value;
        }
    }
};

flags = number %{ _flags = _number; };

exptime = number %{ _expiration = _number; };

bytes = number %{ _blob_size = _number; };

storage_cmd = space key space flags space exptime space bytes space? crlf @blob_start;

set = "set" space key space flags space exptime space bytes space? crlf @blob_start @{ _op = Opcode::Set; };

add = "add" space key space flags space exptime space bytes space? crlf @blob_start @{ _op = Opcode::Add; };

replace = "replace" space key space flags space exptime space bytes space? crlf @blob_start @{ _op = Opcode::Replace; };

get = "get" space key (space key)* crlf @{ _op = Opcode::Get; };

delete = "delete" space key crlf @{ _op = Opcode::Delete; };

delta = number %{ _delta = _number; };

incr = "incr" space key space delta crlf @{ _op = Opcode::Incr; };

decr = "decr" space key space delta crlf @{ _op = Opcode::Decr; };

version = "version" crlf @{ _op = Opcode::Version; };

stats = "stats" crlf @{ _op = Opcode::Stats; };

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

public:
  std::optional<Opcode> _op;
  const char* _key_start = nullptr;
  const char* _key_end = nullptr;
  std::vector<std::string> _owned_keys;
  uint64_t _number = 0;
  bool _number_overflow = false;
  bool _current_number_overflow = false;
  uint64_t _flags = 0;
  uint64_t _expiration = 0;
  const char* _blob_start = nullptr;
  uint64_t _blob_size = 0;
  uint64_t _delta = 0;

  Parser()
  {
    %% write init;
  }

  std::string_view key() const
  {
    if (_owned_keys.empty()) {
      return {};
    }
    return std::string_view{_owned_keys.front().data(), _owned_keys.front().size()};
  }

  const std::vector<std::string>& keys() const
  {
    return _owned_keys;
  }

  uint64_t delta() const
  {
    return _delta;
  }

  size_t parse(std::string_view msg)
  {
    _fsm_cs = start;
    _op.reset();
    _key_start = nullptr;
    _key_end = nullptr;
    _owned_keys.clear();
    _number = 0;
    _number_overflow = false;
    _current_number_overflow = false;
    _flags = 0;
    _expiration = 0;
    _blob_start = nullptr;
    _blob_size = 0;
    _delta = 0;
    if (msg.empty()) {
      return 0;
    }
    auto* start = msg.data();
    auto header_end = msg.find("\r\n");
    if (header_end == std::string_view::npos) {
      // A command header is not complete until CRLF arrives.  Leaving the
      // operation unset tells the reactor to retain the receive buffer.
      return 0;
    }
    auto* parse_end = start + header_end + 2;
    parse(start, parse_end);
    // The parser is deliberately bounded by the first CRLF, so even a
    // complete invalid line consumes exactly that line and cannot eat the
    // following pipelined command.
    return header_end + 2;
  }

private:
  const char* parse(const char *p, const char *pe)
  {
    %% write exec;
    return p;
  }
};

}
