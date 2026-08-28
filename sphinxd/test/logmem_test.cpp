// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/logmem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>

static std::string make_random(size_t len) {
  auto make_random_char = []() {
    thread_local std::minstd_rand rng{1337};
    static constexpr char chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    constexpr size_t nr_chars = sizeof(chars) - 1;
    return chars[rng() % nr_chars];
  };
  std::string str(len, 0);
  std::generate_n(str.begin(), len, make_random_char);
  return str;
}

TEST(LogTest, append) {
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

TEST(LogTest, append_expires) {
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

TEST(LogTest, overwrite_rebinds_index_before_segment_reclamation) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 4 * 64> memory;
  LogConfig cfg{memory.data(), memory.size(), 64};
  Log log{cfg};

  // 每个对象占用一个段。反复覆盖同一个键可同时验证索引键重绑定和旧段回收。
  for (int i = 0; i < 12; i++) {
    auto value = std::string{"value-"} + std::to_string(i);
    ASSERT_TRUE(log.append("same-key", value));
    auto found = log.find("same-key");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found.value(), value);
  }
}

TEST(LogTest, stores_flags_and_expiration) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 128> memory;
  LogConfig cfg{memory.data(), memory.size(), 64};
  Log log{cfg};
  auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
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

TEST(LogTest, remove_handles_missing_expired_and_overwritten_values) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 2048> memory;
  Log log{LogConfig{memory.data(), memory.size(), 128}};

  ASSERT_FALSE(log.remove("missing"));
  ASSERT_TRUE(log.append("key", "old"));
  ASSERT_TRUE(log.append("key", "new"));
  ASSERT_TRUE(log.remove("key"));
  ASSERT_FALSE(log.find_value("key").has_value());
  ASSERT_FALSE(log.remove("key"));
  ASSERT_TRUE(log.append("key", "replacement"));
  ASSERT_EQ(log.find("key").value(), "replacement");

  ASSERT_TRUE(log.append("expired", "value", 0, 1));
  ASSERT_FALSE(log.remove("expired"));
  ASSERT_FALSE(log.find_value("expired").has_value());
}

TEST(LogTest, incr_and_decr_update_decimal_value_and_preserve_metadata) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 4096> memory;
  Log log{LogConfig{memory.data(), memory.size(), 128}};
  auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());

  ASSERT_TRUE(log.append("counter", "0041", 7, now + 3600));
  auto incremented = log.incr("counter", 1);
  ASSERT_EQ(incremented.status, ArithmeticStatus::Success);
  ASSERT_EQ(incremented.value, 42U);
  auto after_increment = log.find_value("counter");
  ASSERT_TRUE(after_increment.has_value());
  ASSERT_EQ(after_increment->blob, "42");
  ASSERT_EQ(after_increment->flags, 7U);
  ASSERT_EQ(after_increment->expiration, now + 3600);

  auto decremented = log.decr("counter", 100);
  ASSERT_EQ(decremented.status, ArithmeticStatus::Success);
  ASSERT_EQ(decremented.value, 0U);
  ASSERT_EQ(log.find("counter").value(), "0");
}

TEST(LogTest, incr_wraps_and_decr_saturates) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 4096> memory;
  Log log{LogConfig{memory.data(), memory.size(), 128}};

  ASSERT_TRUE(log.append("counter", std::to_string(std::numeric_limits<uint64_t>::max())));
  auto wrapped = log.incr("counter", 1);
  ASSERT_EQ(wrapped.status, ArithmeticStatus::Success);
  ASSERT_EQ(wrapped.value, 0U);
  ASSERT_EQ(log.find("counter").value(), "0");

  ASSERT_TRUE(log.append("counter", "3"));
  auto saturated = log.decr("counter", 4);
  ASSERT_EQ(saturated.status, ArithmeticStatus::Success);
  ASSERT_EQ(saturated.value, 0U);
  ASSERT_EQ(log.find("counter").value(), "0");
}

TEST(LogTest, arithmetic_rejects_missing_expired_and_non_numeric_values) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 4096> memory;
  Log log{LogConfig{memory.data(), memory.size(), 128}};

  ASSERT_EQ(log.incr("missing", 1).status, ArithmeticStatus::NotFound);
  ASSERT_TRUE(log.append("text", "12x"));
  auto invalid = log.decr("text", 1);
  ASSERT_EQ(invalid.status, ArithmeticStatus::NonNumeric);
  ASSERT_EQ(log.find("text").value(), "12x");

  ASSERT_TRUE(log.append("empty", ""));
  ASSERT_EQ(log.incr("empty", 1).status, ArithmeticStatus::NonNumeric);
  ASSERT_TRUE(log.append("signed", "+1"));
  ASSERT_EQ(log.incr("signed", 1).status, ArithmeticStatus::NonNumeric);
  ASSERT_TRUE(log.append("overflow", "18446744073709551616"));
  ASSERT_EQ(log.incr("overflow", 1).status, ArithmeticStatus::NonNumeric);

  ASSERT_TRUE(log.append("expired", "1", 0, 1));
  ASSERT_EQ(log.decr("expired", 1).status, ArithmeticStatus::NotFound);
}

TEST(LogTest, arithmetic_rejects_non_decimal_values_without_mutating_metadata) {
  using namespace sphinx::logmem;
  alignas(std::max_align_t) std::array<char, 8192> memory;
  Log log{LogConfig{memory.data(), memory.size(), 128}};
  auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());

  // 计数器只接受非空 ASCII 十进制数字序列。
  // 特别是，符号、空白和十进制表示法不能在 incr/decr 中被静默规范化。
  static constexpr std::array<std::string_view, 8> invalid_values = {
      "-1", "+1", " 1", "1 ", "\t1", "1\n", "1.0", "0x10",
  };
  for (size_t i = 0; i < invalid_values.size(); i++) {
    auto key = std::string{"invalid-decimal-"} + std::to_string(i);
    const auto flags = static_cast<uint32_t>(100 + i);
    const auto expiration = now + 3600 + i;
    ASSERT_TRUE(log.append(key, invalid_values[i], flags, expiration));

    auto incremented = log.incr(key, 1);
    ASSERT_EQ(incremented.status, ArithmeticStatus::NonNumeric);
    auto decremented = log.decr(key, 1);
    ASSERT_EQ(decremented.status, ArithmeticStatus::NonNumeric);

    auto unchanged = log.find_value(key);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->blob, invalid_values[i]);
    EXPECT_EQ(unchanged->flags, flags);
    EXPECT_EQ(unchanged->expiration, expiration);
  }

  // 拒绝无效值不应让已过期对象重新可见；两种操作仍应视为普通未命中。
  ASSERT_TRUE(log.append("expired-invalid", "-1", 77, 1));
  ASSERT_FALSE(log.find_value("expired-invalid").has_value());
  ASSERT_EQ(log.incr("expired-invalid", 1).status, ArithmeticStatus::NotFound);
  ASSERT_EQ(log.decr("expired-invalid", 1).status, ArithmeticStatus::NotFound);
  ASSERT_FALSE(log.find_value("expired-invalid").has_value());
}
