// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/logmem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <type_traits>

using sphinx::Log;
using sphinx::LogConfig;

static_assert(!std::is_copy_constructible_v<Log>);
static_assert(!std::is_copy_assignable_v<Log>);
static_assert(!std::is_move_constructible_v<Log>);
static_assert(!std::is_move_assignable_v<Log>);

static std::string make_random(size_t len) {
  auto make_random_char = []() {
    thread_local std::minstd_rand rng{std::random_device{}()};
    static constexpr char chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    constexpr size_t nr_chars = sizeof(chars) - 1;
    return chars[rng() % nr_chars];
  };
  std::string str(len, 0);
  std::generate_n(str.begin(), len, make_random_char);
  return str;
}

TEST(LogTest, Append) {
  std::array<char, 128> memory;
  LogConfig cfg;
  cfg.segment_size = 64;
  cfg.memory_ptr = memory.data();
  cfg.memory_size = memory.size();
  Log log{cfg};
  auto key = make_random(8);
  auto blob = make_random(16);
  log.append(key, blob);
  auto blob_opt = log.find_value(key);
  if (!blob_opt) {
    FAIL() << "appended value was not found";
    return;
  }
  ASSERT_EQ(blob_opt->blob, blob);
}

TEST(LogTest, append_expires) {
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

TEST(LogTest, OverwriteRebindsIndexBeforeSegmentReclamation) {
  alignas(std::max_align_t) std::array<char, size_t{4} * 64> memory;
  LogConfig const cfg{memory.data(), memory.size(), 64};
  Log log{cfg};

  // 每个对象占用一个段。反复覆盖同一个键可同时验证索引键重绑定和旧段回收。
  for (int i = 0; i < 12; i++) {
    auto value = std::string{"value-"} + std::to_string(i);
    ASSERT_TRUE(log.append("same-key", value));
    auto found = log.find_value("same-key");
    if (!found) {
      FAIL() << "overwritten value was not found";
      return;
    }
    ASSERT_EQ(found->blob, value);
  }
}

TEST(LogTest, StoresFlagsAndExpiration) {
  alignas(std::max_align_t) std::array<char, 128> memory;
  LogConfig const cfg{memory.data(), memory.size(), 64};
  Log log{cfg};
  auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());

  ASSERT_TRUE(log.append("metadata", "payload", 123, now + 3600));
  auto value = log.find_value("metadata");
  if (!value) {
    FAIL() << "value with metadata was not found";
    return;
  }
  ASSERT_EQ(value->flags, 123U);
  ASSERT_EQ(value->blob, "payload");
  ASSERT_EQ(value->expiration, now + 3600);

  ASSERT_TRUE(log.append("expired", "payload", 7, 1));
  ASSERT_FALSE(log.find_value("expired").has_value());
}

TEST(LogTest, RemoveHandlesMissingExpiredAndOverwrittenValues) {
  alignas(std::max_align_t) std::array<char, 2048> memory;
  Log log{LogConfig{memory.data(), memory.size(), 128}};

  ASSERT_FALSE(log.remove("missing"));
  ASSERT_TRUE(log.append("key", "old"));
  ASSERT_TRUE(log.append("key", "new"));
  ASSERT_TRUE(log.remove("key"));
  ASSERT_FALSE(log.find_value("key").has_value());
  ASSERT_FALSE(log.remove("key"));
  ASSERT_TRUE(log.append("key", "replacement"));
  const auto replacement = log.find_value("key");
  if (!replacement) {
    FAIL() << "replacement value was not found";
    return;
  }
  ASSERT_EQ(replacement->blob, "replacement");

  ASSERT_TRUE(log.append("expired", "value", 0, 1));
  ASSERT_FALSE(log.remove("expired"));
  ASSERT_FALSE(log.find_value("expired").has_value());
}
