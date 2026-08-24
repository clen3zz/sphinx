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
#include <string_view>

%%{

machine memcache_protocol;

access _fsm_;

action key_start {
    _key_start = p;
}

action key_end {
    _key_end = p;
}

action blob_start {
    _blob_start = p + 1;
}

crlf = "\r\n";

key = [^ ]+ >key_start %key_end;

number = digit+ >{ _number = 0; _number_overflow = false; } ${
    if (!_number_overflow) {
        auto digit_value = uint64_t(fc - '0');
        if (_number > (std::numeric_limits<uint64_t>::max() - digit_value) / 10) {
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

get = "get" space key crlf @{ _op = Opcode::Get; };

version = "version" crlf @{ _op = Opcode::Version; };

main := (set | add | replace | get | version);

}%%

namespace sphinx::memcache {

%% write data nofinal noprefix;

enum class Opcode
{
  Set,
  Add,
  Replace,
  Get,
  Version,
};

class Parser
{
  int _fsm_cs;

public:
  std::optional<Opcode> _op;
  const char* _key_start = nullptr;
  const char* _key_end = nullptr;
  uint64_t _number = 0;
  bool _number_overflow = false;
  uint64_t _flags = 0;
  uint64_t _expiration = 0;
  const char* _blob_start = nullptr;
  uint64_t _blob_size = 0;

  Parser()
  {
    %% write init;
  }

  std::string_view key() const
  {
    std::string_view::size_type key_size = _key_end - _key_start;
    return std::string_view{_key_start, key_size};
  }

  size_t parse(std::string_view msg)
  {
    if (msg.empty()) {
      return 0;
    }
    auto* start = msg.data();
    auto* end = start + msg.size();
    // Parse exactly one command header.  Ragel's default scanner may continue
    // into a pipelined command after the first accepting state; bounding the
    // execution range at the first CRLF keeps the returned offset and all
    // captured fields tied to this command.
    auto header_end = msg.find("\r\n");
    auto* parse_end = header_end == std::string_view::npos ? end : start + header_end + 2;
    auto* next = parse(start, parse_end);
    if (start != next) {
      return next - start;
    }
    return end - start;
  }

private:
  const char* parse(const char *p, const char *pe)
  {
    %% write exec;
    return p;
  }
};

}
