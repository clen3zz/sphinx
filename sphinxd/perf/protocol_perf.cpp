// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>
#include <sphinx/protocol.h>

#include <string>

static void Protocol_parse(benchmark::State& state) {
  std::string msg = "get QeYm4XMK\r\n";
  for (auto _ : state) {
    sphinx::Parser parser;
    parser.parse(msg);
    benchmark::DoNotOptimize(parser.command().has_value());
    benchmark::ClobberMemory();
  }
}
BENCHMARK(Protocol_parse);
