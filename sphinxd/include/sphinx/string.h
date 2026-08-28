// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace sphinx {

inline std::string to_string(unsigned long n) {
  if (n == 0) {
    return "0";
  }

  constexpr std::size_t max_digits = 20;
  std::array<char, max_digits> ret;
  std::size_t offset = max_digits;
  while (n > 0) {
    auto digit = n % 10;
    n = (n - digit) / 10;
    ret[--offset] = static_cast<char>('0' + digit);
  }
  return std::string{ret.data() + offset, max_digits - offset};
}
}  // namespace sphinx
