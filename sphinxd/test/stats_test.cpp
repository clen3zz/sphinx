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

#include <sphinx/stats.h>

#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using ServerStats = sphinx::stats::ServerStats;
using Counter = ServerStats::Counter;

constexpr std::array<Counter, 9> kCounters = {
  Counter::CmdGet,
  Counter::GetHits,
  Counter::GetMisses,
  Counter::CmdSet,
  Counter::CmdAdd,
  Counter::CmdReplace,
  Counter::CmdDelete,
  Counter::CmdIncr,
  Counter::CmdDecr,
};

TEST(ServerStatsTest, CountersStartAtZero)
{
  ServerStats stats{"v-test", 4, 64 * 1024 * 1024};

  for (auto counter : kCounters) {
    EXPECT_EQ(stats.counter(counter), 0U);
  }

  EXPECT_EQ(stats.render(),
            "STAT version v-test\r\n"
            "STAT threads 4\r\n"
            "STAT limit_maxbytes 67108864\r\n"
            "STAT cmd_get 0\r\n"
            "STAT get_hits 0\r\n"
            "STAT get_misses 0\r\n"
            "STAT cmd_set 0\r\n"
            "STAT cmd_add 0\r\n"
            "STAT cmd_replace 0\r\n"
            "STAT cmd_delete 0\r\n"
            "STAT cmd_incr 0\r\n"
            "STAT cmd_decr 0\r\n"
            "END\r\n");
}

TEST(ServerStatsTest, RenderUsesFixedOrder)
{
  ServerStats stats{ServerStats::Config{"v-fixed", 8, 123456789}};
  stats.record_get_command();
  stats.record_get_hit();
  stats.record_get_miss();
  stats.record_set_command();
  stats.record_add_command();
  stats.record_replace_command();
  stats.record_delete_command();
  stats.record_incr_command();
  stats.record_decr_command();

  EXPECT_EQ(stats.render(),
            "STAT version v-fixed\r\n"
            "STAT threads 8\r\n"
            "STAT limit_maxbytes 123456789\r\n"
            "STAT cmd_get 1\r\n"
            "STAT get_hits 1\r\n"
            "STAT get_misses 1\r\n"
            "STAT cmd_set 1\r\n"
            "STAT cmd_add 1\r\n"
            "STAT cmd_replace 1\r\n"
            "STAT cmd_delete 1\r\n"
            "STAT cmd_incr 1\r\n"
            "STAT cmd_decr 1\r\n"
            "END\r\n");
}

TEST(ServerStatsTest, ConcurrentIncrementsAreExact)
{
  ServerStats stats{"v-concurrent", 8, 1};
  constexpr size_t kThreadCount = 8;
  constexpr size_t kIterations = 25000;
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (size_t i = 0; i < kThreadCount; i++) {
    workers.emplace_back([&stats] {
      for (size_t j = 0; j < kIterations; j++) {
        for (auto counter : kCounters) {
          stats.increment(counter);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  constexpr uint64_t expected = kThreadCount * kIterations;
  for (auto counter : kCounters) {
    EXPECT_EQ(stats.counter(counter), expected);
  }
}

} // namespace
