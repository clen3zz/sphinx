// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/string.h>

#include <random>
#include <string>

TEST(StringTest, to_string) {
  std::minstd_rand rng{1337};
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(std::to_string(i), sphinx::to_string(static_cast<unsigned long>(i)));
    auto v = static_cast<int>(rng());
    ASSERT_EQ(std::to_string(v), sphinx::to_string(static_cast<unsigned long>(v)));
  }
}
