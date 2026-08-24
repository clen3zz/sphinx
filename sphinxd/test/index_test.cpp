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

#include <gtest/gtest.h>

#include <sphinx/index.h>

#include <string>

TEST(IndexTest, overwriteReturnsOldValueAndRebindsViewKey)
{
  sphinx::index::Index<std::string_view, int> index;
  std::string first = "same";
  std::string second = "same";
  ASSERT_FALSE(index.insert_or_assign(first, 1).has_value());
  auto old = index.insert_or_assign(second, 2);
  ASSERT_TRUE(old.has_value());
  ASSERT_EQ(old.value(), 1);
  ASSERT_EQ(index.find(std::string_view{"same"}).value(), 2);

  // The replacement key is the second view, not the stale first view.
  first.assign("xxxx");
  ASSERT_EQ(index.find(std::string_view{"same"}).value(), 2);
}
