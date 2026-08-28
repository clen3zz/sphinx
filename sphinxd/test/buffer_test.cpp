// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/buffer.h>

#include <algorithm>

TEST(BufferTest, append) {
  using namespace sphinx::buffer;
  Buffer buf;
  ASSERT_TRUE(buf.size() == 0);
  std::string value = "The quick brown fox jumps over the lazy dog";
  buf.append(value);
  ASSERT_EQ(value.size(), buf.size());
  ASSERT_EQ(value, buf.string_view());
}
