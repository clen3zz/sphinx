// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/buffer.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sphinx::buffer {

bool
Buffer::is_empty() const
{
  return _data.empty();
}

void
Buffer::append(std::string_view data)
{
  if (data.empty()) {
    return;
  }
  _data.insert(_data.end(), data.data(), data.data() + data.size());
}

void
Buffer::remove_prefix(size_t n)
{
  if (n > _data.size()) {
    throw std::out_of_range("buffer prefix is larger than the buffer");
  }
  using DifferenceType = std::vector<char>::difference_type;
  _data.erase(_data.begin(), _data.begin() + static_cast<DifferenceType>(n));
}

const char*
Buffer::data() const
{
  return _data.data();
}

size_t
Buffer::size() const
{
  return _data.size();
}

std::string_view
Buffer::string_view() const
{
  return std::string_view{_data.data(), _data.size()};
}
} // namespace sphinx::buffer
