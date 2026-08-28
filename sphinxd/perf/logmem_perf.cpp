// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <sphinx/logmem.h>
#include <sphinx/memory.h>

#include <algorithm>
#include <cstdlib>
#include <string>

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

static void
Log_append_expiring(benchmark::State& state)
{
  using namespace sphinx::memory;
  using namespace sphinx::logmem;
  size_t mem_size = size_t{2} * 1024 * 1024;
  size_t segment_size = size_t{1} * 1024 * 1024;
  Memory memory = Memory::mmap(mem_size);
  LogConfig cfg;
  cfg.segment_size = segment_size;
  cfg.memory_ptr = reinterpret_cast<char*>(memory.addr());
  cfg.memory_size = memory.size();
  Log log{cfg};
  std::string key = make_random(8);
  std::string blob = make_random(state.range(0));
  for (auto _ : state) {
    log.append(key, blob);
  }
}
BENCHMARK(Log_append_expiring)->RangeMultiplier(2)->Range(8, 8 << 10);
