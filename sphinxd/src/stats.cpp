// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/stats.h>

#include <iterator>
#include <utility>

namespace sphinx::stats {

namespace {

using Counter = ServerStats::Counter;

struct CounterInfo {
  Counter counter;
  const char* name;
};

constexpr std::array<CounterInfo, static_cast<size_t>(Counter::Count)> kCounterNames = {{
    {Counter::CmdGet, "cmd_get"},
    {Counter::GetHits, "get_hits"},
    {Counter::GetMisses, "get_misses"},
    {Counter::CmdSet, "cmd_set"},
    {Counter::CmdAdd, "cmd_add"},
    {Counter::CmdReplace, "cmd_replace"},
    {Counter::CmdDelete, "cmd_delete"},
    {Counter::CmdIncr, "cmd_incr"},
    {Counter::CmdDecr, "cmd_decr"},
}};

static_assert(std::size(kCounterNames) == static_cast<size_t>(Counter::Count));

}  // namespace

ServerStats::ServerStats(Config config)
    : _version{std::move(config.version)},
      _threads{config.threads},
      _limit_maxbytes{config.limit_maxbytes} {
}

ServerStats::ServerStats(std::string version, uint64_t threads, uint64_t limit_maxbytes)
    : ServerStats(Config{std::move(version), threads, limit_maxbytes}) {
}

void ServerStats::increment(Counter counter, uint64_t amount) noexcept {
  const auto index = static_cast<size_t>(counter);
  if (index < _counters.size()) {
    _counters[index].fetch_add(amount, std::memory_order_relaxed);
  }
}

uint64_t ServerStats::counter(Counter counter) const noexcept {
  const auto index = static_cast<size_t>(counter);
  if (index < _counters.size()) {
    return _counters[index].load(std::memory_order_relaxed);
  }
  return 0;
}

std::string ServerStats::render() const {
  std::string response;
  response.reserve(256);

  response += "STAT version ";
  response += _version;
  response += "\r\n";
  response += "STAT threads ";
  response += std::to_string(_threads);
  response += "\r\n";
  response += "STAT limit_maxbytes ";
  response += std::to_string(_limit_maxbytes);
  response += "\r\n";
  for (const auto& [counter_type, name] : kCounterNames) {
    response += "STAT ";
    response += name;
    response += ' ';
    response += std::to_string(counter(counter_type));
    response += "\r\n";
  }
  response += "END\r\n";
  return response;
}

}  // namespace sphinx::stats
