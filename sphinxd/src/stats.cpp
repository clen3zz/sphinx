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

#include <sphinx/stats.h>

#include <utility>

namespace sphinx::stats {

ServerStats::ServerStats(Config config)
  : _version{std::move(config.version)}
  , _threads{config.threads}
  , _limit_maxbytes{config.limit_maxbytes}
{
}

ServerStats::ServerStats(std::string version, uint64_t threads, uint64_t limit_maxbytes)
  : ServerStats(Config{std::move(version), threads, limit_maxbytes})
{
}

void
ServerStats::increment(Counter counter, uint64_t amount) noexcept
{
  switch (counter) {
    case Counter::CmdGet:
      _cmd_get.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::GetHits:
      _get_hits.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::GetMisses:
      _get_misses.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::CmdSet:
      _cmd_set.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::CmdAdd:
      _cmd_add.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::CmdReplace:
      _cmd_replace.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::CmdDelete:
      _cmd_delete.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::CmdIncr:
      _cmd_incr.fetch_add(amount, std::memory_order_relaxed);
      break;
    case Counter::CmdDecr:
      _cmd_decr.fetch_add(amount, std::memory_order_relaxed);
      break;
  }
}

void
ServerStats::record_get_command() noexcept
{
  increment(Counter::CmdGet);
}

void
ServerStats::record_get_hit() noexcept
{
  increment(Counter::GetHits);
}

void
ServerStats::record_get_miss() noexcept
{
  increment(Counter::GetMisses);
}

void
ServerStats::record_set_command() noexcept
{
  increment(Counter::CmdSet);
}

void
ServerStats::record_add_command() noexcept
{
  increment(Counter::CmdAdd);
}

void
ServerStats::record_replace_command() noexcept
{
  increment(Counter::CmdReplace);
}

void
ServerStats::record_delete_command() noexcept
{
  increment(Counter::CmdDelete);
}

void
ServerStats::record_incr_command() noexcept
{
  increment(Counter::CmdIncr);
}

void
ServerStats::record_decr_command() noexcept
{
  increment(Counter::CmdDecr);
}

uint64_t
ServerStats::counter(Counter counter) const noexcept
{
  switch (counter) {
    case Counter::CmdGet:
      return _cmd_get.load(std::memory_order_relaxed);
    case Counter::GetHits:
      return _get_hits.load(std::memory_order_relaxed);
    case Counter::GetMisses:
      return _get_misses.load(std::memory_order_relaxed);
    case Counter::CmdSet:
      return _cmd_set.load(std::memory_order_relaxed);
    case Counter::CmdAdd:
      return _cmd_add.load(std::memory_order_relaxed);
    case Counter::CmdReplace:
      return _cmd_replace.load(std::memory_order_relaxed);
    case Counter::CmdDelete:
      return _cmd_delete.load(std::memory_order_relaxed);
    case Counter::CmdIncr:
      return _cmd_incr.load(std::memory_order_relaxed);
    case Counter::CmdDecr:
      return _cmd_decr.load(std::memory_order_relaxed);
  }
  return 0;
}

std::string
ServerStats::render() const
{
  std::string response;
  response.reserve(256);
  auto append_counter = [&response, this](const char* name, Counter counter) {
    response += "STAT ";
    response += name;
    response += " ";
    response += std::to_string(this->counter(counter));
    response += "\r\n";
  };

  response += "STAT version ";
  response += _version;
  response += "\r\n";
  response += "STAT threads ";
  response += std::to_string(_threads);
  response += "\r\n";
  response += "STAT limit_maxbytes ";
  response += std::to_string(_limit_maxbytes);
  response += "\r\n";
  append_counter("cmd_get", Counter::CmdGet);
  append_counter("get_hits", Counter::GetHits);
  append_counter("get_misses", Counter::GetMisses);
  append_counter("cmd_set", Counter::CmdSet);
  append_counter("cmd_add", Counter::CmdAdd);
  append_counter("cmd_replace", Counter::CmdReplace);
  append_counter("cmd_delete", Counter::CmdDelete);
  append_counter("cmd_incr", Counter::CmdIncr);
  append_counter("cmd_decr", Counter::CmdDecr);
  response += "END\r\n";
  return response;
}

} // namespace sphinx::stats
