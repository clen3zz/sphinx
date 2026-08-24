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

#include <sphinx/logmem.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

static std::string
make_random(size_t len)
{
  auto make_random_char = []() {
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const size_t nr_chars = sizeof(chars) - 1;
    return chars[rand() % nr_chars];
  };
  std::string str(len, 0);
  std::generate_n(str.begin(), len, make_random_char);
  return str;
}

TEST(LogTest, append)
{
  using namespace sphinx::logmem;
  std::array<char, 128> memory;
  LogConfig cfg;
  cfg.segment_size = 64;
  cfg.memory_ptr = memory.data();
  cfg.memory_size = memory.size();
  Log log{cfg};
  auto key = make_random(8);
  auto blob = make_random(16);
  log.append(key, blob);
  auto blob_opt = log.find(key);
  ASSERT_TRUE(blob_opt.has_value());
  ASSERT_EQ(blob_opt.value(), blob);
}

TEST(LogTest, append_expires)
{
  using namespace sphinx::logmem;
  std::array<char, 1024> memory;
  LogConfig cfg;
  cfg.segment_size = 64;
  cfg.memory_ptr = memory.data();
  cfg.memory_size = memory.size();
  Log log{cfg};
  std::string key;
  std::string blob;
  for (int i = 0; i < 10; i++) {
    key = make_random(8);
    blob = make_random(16);
    ASSERT_TRUE(log.append(key, blob));
  }
}

TEST(LogTest, overwrite_rebinds_index_before_segment_reclamation)
{
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 4 * 64> memory;
  LogConfig cfg{memory.data(), memory.size(), 64};
  Log log{cfg};

  // Each object occupies one segment.  Repeatedly overwriting the same key
  // therefore exercises both index-key rebinding and old-segment reclamation.
  for (int i = 0; i < 12; i++) {
    auto value = std::string{"value-"} + std::to_string(i);
    ASSERT_TRUE(log.append("same-key", value));
    auto found = log.find("same-key");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found.value(), value);
  }
}

TEST(LogTest, stores_flags_and_expiration)
{
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 128> memory;
  LogConfig cfg{memory.data(), memory.size(), 64};
  Log log{cfg};
  auto now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());

  ASSERT_TRUE(log.append("metadata", "payload", 123, now + 3600));
  auto value = log.find_value("metadata");
  ASSERT_TRUE(value.has_value());
  ASSERT_EQ(value->flags, 123U);
  ASSERT_EQ(value->blob, "payload");
  ASSERT_EQ(value->expiration, now + 3600);

  ASSERT_TRUE(log.append("expired", "payload", 7, 1));
  ASSERT_FALSE(log.find_value("expired").has_value());
}
