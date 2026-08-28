// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sphinx {

/**
 * 用于生成简易 ASCII stats 响应的进程级计数器。
 *
 * 每个工作线程都应共享同一个 ServerStats 实例。计数器更新有意采用 relaxed
 * 顺序：stats 是弱一致性观察，不参与请求同步。
 */
class ServerStats {
 public:
  struct Config {
    std::string version;
    uint64_t threads = 0;
    uint64_t limit_maxbytes = 0;
  };

  enum class Counter : uint8_t {
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

  // 返回固定顺序的 Memcached ASCII stats 响应。
  std::string render() const;

 private:
  std::string _version;
  uint64_t _threads;
  uint64_t _limit_maxbytes;

  static constexpr size_t counter_count = static_cast<size_t>(Counter::Count);
  std::array<std::atomic<uint64_t>, counter_count> _counters{};
};

}  // namespace sphinx
