// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>
#include <vector>

namespace sphinx::buffer {

class Buffer {
  std::vector<char> _data;

 public:
  bool is_empty() const;
  void append(std::string_view data);
  void remove_prefix(size_t n);
  const char* data() const;
  size_t size() const;
  std::string_view string_view() const;
};
}  // namespace sphinx::buffer

namespace sphinx {
using Buffer = buffer::Buffer;
}
