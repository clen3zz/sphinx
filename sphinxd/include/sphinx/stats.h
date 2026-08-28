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

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sphinx::stats {

/**
 * Process-wide counters used by the small ASCII stats response.
 *
 * A ServerStats instance is intended to be shared by every worker.  Counter
 * updates are deliberately relaxed: stats is a weakly-consistent observation
 * and does not participate in request synchronization.
 */
class ServerStats
{
public:
  struct Config
  {
    std::string version;
    uint64_t threads = 0;
    uint64_t limit_maxbytes = 0;
  };

  enum class Counter : uint8_t
  {
    CmdGet,
    GetHits,
    GetMisses,
    CmdSet,
    CmdAdd,
    CmdReplace,
    CmdDelete,
    CmdIncr,
    CmdDecr,
    Count,
  };

  explicit ServerStats(Config config);
  ServerStats(std::string version, uint64_t threads, uint64_t limit_maxbytes);

  ServerStats(const ServerStats&) = delete;
  ServerStats& operator=(const ServerStats&) = delete;
  ServerStats(ServerStats&&) = delete;
  ServerStats& operator=(ServerStats&&) = delete;

  void increment(Counter counter, uint64_t amount = 1) noexcept;

  uint64_t counter(Counter counter) const noexcept;

  // Return the fixed-order Memcached ASCII stats response.
  std::string render() const;

private:
  std::string _version;
  uint64_t _threads;
  uint64_t _limit_maxbytes;

  static constexpr size_t counter_count = static_cast<size_t>(Counter::Count);
  std::array<std::atomic<uint64_t>, counter_count> _counters{};
};

} // namespace sphinx::stats

namespace sphinx {
using ServerStats = stats::ServerStats;
}
