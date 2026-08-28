// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>
#include <sphinx/buffer.h>

#include <algorithm>
#include <cstdlib>
#include <string>

static std::string make_random(size_t len) {
  auto make_random_char = []() {
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const size_t nr_chars = sizeof(chars) - 1;
    return chars[rand() % nr_chars];
  };
  std::string str(len, 0);
  std::generate_n(str.begin(), len, make_random_char);
  return str;
}

static void Buffer_append(benchmark::State& state) {
  sphinx::Buffer buf;
  std::string value = make_random(state.range(0));
  for (auto _ : state) {
    buf.append(value);
  }
}
BENCHMARK(Buffer_append)->RangeMultiplier(2)->Range(8, 8 << 10);
