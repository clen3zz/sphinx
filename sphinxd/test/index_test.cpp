// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/index.h>

#include <optional>
#include <string>

TEST(IndexTest, overwriteReturnsOldValueAndRebindsViewKey) {
  sphinx::Index<std::string_view, int> index;
  std::string first = "same";
  std::string second = "same";
  ASSERT_FALSE(index.insert_or_assign(first, 1).has_value());
  auto old = index.insert_or_assign(second, 2);
  ASSERT_TRUE(old.has_value());
  ASSERT_EQ(old, std::optional{1});
  ASSERT_EQ(index.find(std::string_view{"same"}), std::optional{2});

  // 替换后的键应是第二个视图，而不是已经失效的第一个视图。
  first.assign("xxxx");
  ASSERT_EQ(index.find(std::string_view{"same"}), std::optional{2});
}
