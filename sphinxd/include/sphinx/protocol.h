// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/protocol_types.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sphinx {

enum class Opcode { Set, Get, Delete, Version, Stats };

// Parse one complete Memcached text header at a time. The SET body remains in
// the connection buffer and is described by StorageBody rather than copied here.
class Parser {
 public:
  const std::optional<ParsedCommand>& command() const noexcept { return _command; }
  ParseStatus status() const noexcept { return _status; }
  bool number_overflow() const noexcept { return _number_overflow; }

  size_t parse(std::string_view input) {
    _command.reset();
    _status = ParseStatus::Incomplete;
    _number_overflow = false;

    const auto line_end = input.find("\r\n");
    if (line_end == std::string_view::npos) {
      return 0;
    }
    const size_t header_size = line_end + 2;
    const auto fields = split_fields(input.substr(0, line_end));
    if (!fields.empty()) {
      if (fields[0] == "set" && fields.size() == 5 && valid_key(fields[1])) {
        uint64_t flags = 0;
        uint64_t expiration = 0;
        uint64_t body_size = 0;
        if (parse_number(fields[2], flags) && parse_number(fields[3], expiration) &&
            parse_number(fields[4], body_size)) {
          _command = SetCommand{std::string{fields[1]}, flags, expiration,
                                StorageBody{body_size, header_size}};
        }
      } else if (fields[0] == "get" && fields.size() >= 2) {
        std::vector<std::string> keys;
        keys.reserve(fields.size() - 1);
        for (size_t i = 1; i < fields.size(); ++i) {
          if (!valid_key(fields[i])) {
            keys.clear();
            break;
          }
          keys.emplace_back(fields[i]);
        }
        if (!keys.empty()) {
          _command = GetCommand{std::move(keys)};
        }
      } else if (fields[0] == "delete" && fields.size() == 2 && valid_key(fields[1])) {
        _command = DeleteCommand{std::string{fields[1]}};
      } else if (fields[0] == "version" && fields.size() == 1) {
        _command = VersionCommand{};
      } else if (fields[0] == "stats" && fields.size() == 1) {
        _command = StatsCommand{};
      }
    }
    _status = _command ? ParseStatus::Parsed : ParseStatus::Invalid;
    return header_size;
  }

 private:
  static std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    size_t begin = 0;
    while (begin < line.size()) {
      while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
        ++begin;
      }
      if (begin == line.size()) {
        break;
      }
      size_t end = begin;
      while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
        ++end;
      }
      fields.push_back(line.substr(begin, end - begin));
      begin = end;
    }
    return fields;
  }

  static bool valid_key(std::string_view key) {
    if (key.empty()) {
      return false;
    }
    for (const unsigned char ch : key) {
      if (ch <= ' ' || ch == 127) {
        return false;
      }
    }
    return true;
  }

  bool parse_number(std::string_view token, uint64_t& value) {
    if (token.empty()) {
      return false;
    }
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec == std::errc::result_out_of_range) {
      _number_overflow = true;
      return false;
    }
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
  }

  std::optional<ParsedCommand> _command;
  ParseStatus _status = ParseStatus::Incomplete;
  bool _number_overflow = false;
};

}  // namespace sphinx
