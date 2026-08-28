// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sphinx {

// 命令头解析与接收命令值是两个刻意分开的操作。解析器每次消费一个以 CRLF
// 结尾的命令头；存储命令的 Body 描述后续值的预期位置。
struct StorageBody {
  uint64_t size = 0;
  size_t offset = 0;

  // 协议帧由值及其末尾的 CRLF 组成。这里返回 nullopt 很重要：uint64 长度
  // 可能无法放入 size_t，追加 CRLF 时也不能发生整数回绕。
  std::optional<size_t> frame_size() const noexcept {
    constexpr auto max_size = std::numeric_limits<size_t>::max();
    if (size > max_size - 2) {
      return std::nullopt;
    }
    return static_cast<size_t>(size) + 2;
  }

  bool available(std::string_view input) const noexcept {
    const auto frame = frame_size();
    return frame.has_value() && offset <= input.size() && *frame <= input.size() - offset;
  }

  bool has_valid_terminator(std::string_view input) const noexcept {
    if (!available(input)) {
      return false;
    }
    const auto value_size = static_cast<size_t>(size);
    return input[offset + value_size] == '\r' && input[offset + value_size + 1] == '\n';
  }

  // 仅当包含末尾 CRLF 的完整帧存在且有效时才返回值。返回空结果表示接收缓冲区
  // 需要更多字节，或帧格式错误/长度过大。
  std::optional<std::string_view> view(std::string_view input) const noexcept {
    if (!has_valid_terminator(input)) {
      return std::nullopt;
    }
    return input.substr(offset, size);
  }
};

struct SetCommand {
  std::string key;
  uint64_t flags = 0;
  uint64_t expiration = 0;
  StorageBody body;
};

struct AddCommand {
  std::string key;
  uint64_t flags = 0;
  uint64_t expiration = 0;
  StorageBody body;
};

struct ReplaceCommand {
  std::string key;
  uint64_t flags = 0;
  uint64_t expiration = 0;
  StorageBody body;
};

struct GetCommand {
  std::vector<std::string> keys;
};

struct DeleteCommand {
  std::string key;
};

struct IncrCommand {
  std::string key;
  uint64_t delta = 0;
};

struct DecrCommand {
  std::string key;
  uint64_t delta = 0;
};

struct VersionCommand {};

struct StatsCommand {};

// 解析后的命令拥有全部文本字段。特别是，GetCommand 的键不指向接收缓冲区，
// 因为解析器返回后 reactor 可能会立即压缩或释放该缓冲区。
using ParsedCommand =
    std::variant<SetCommand, AddCommand, ReplaceCommand, GetCommand, DeleteCommand, IncrCommand,
                 DecrCommand, VersionCommand, StatsCommand>;

enum class ParseStatus : uint8_t {
  Incomplete,
  Invalid,
  Parsed,
};

}  // namespace sphinx
