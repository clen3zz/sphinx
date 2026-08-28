// SPDX-License-Identifier: Apache-2.0
#include <getopt.h>
#include <sched.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "config.h"

namespace sphinx::server {
namespace {
void print_version() {
  std::cout << "Sphinx " << SPHINX_VERSION << '\n' << std::flush;
}
void print_usage(const std::string& program) {
  std::cout
      << "Usage: " << program << " [OPTION]...\n"
      << "Start the Sphinx daemon.\n\nOptions:\n"
      << "  -p, --port number           TCP port to listen to (default: " << default_tcp_port
      << ")\n  -l, --listen address        interface to listen to (default: " << default_listen_addr
      << ")\n  -m, --memory-limit number   Memory limit in MB (default: " << default_memory_limit
      << ")\n  -s, --segment-size number   Segment size in MB (default: " << default_segment_size
      << ")\n  -b, --listen-backlog number Listen backlog size (default: " << default_listen_backlog
      << ")\n  -t, --threads number        number of threads to use (default: "
      << default_nr_threads << ")\n  -I, --io-backend name       I/O backend (default: "
      << sphinx::reactor::Reactor::default_backend()
      << ")\n  -i, --isolate-cpus list     list of CPUs to isolate application threads\n"
      << "  -S, --sched-fifo            use SCHED_FIFO scheduling policy\n"
      << "      --help                  print this help text and exit\n"
      << "      --version               print Sphinx version and exit\n\n";
}
void print_option_error(const std::string& program, const std::string& option,
                        const std::string& reason) {
  std::cerr << program << ": " << reason << " '" << option << "' option\n";
  std::cerr << "Try '" << program << " --help' for more information\n" << std::flush;
}
std::set<int> parse_cpu_list(const std::string& raw_cpu_list) {
  std::set<int> cpu_list;
  std::istringstream input{raw_cpu_list};
  std::string token;
  while (std::getline(input, token, ',')) {
    auto cpu = std::stoi(token);
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
      throw std::invalid_argument("CPU id is out of range");
    }
    cpu_list.emplace(cpu);
  }
  return cpu_list;
}
void validate(const Config& args) {
  const auto require = [](bool valid, const std::string& message) {
    if (!valid) {
      throw std::invalid_argument(message);
    }
  };
  require(args.tcp_port >= 0 && args.tcp_port <= 65535, "TCP port must be between 0 and 65535");
  require(args.memory_limit > 0, "memory limit must be positive");
  require(args.segment_size > 0, "segment size must be positive");
  require(args.listen_backlog > 0, "listen backlog must be positive");
  require(args.nr_threads > 0 && args.nr_threads <= sphinx::reactor::max_nr_threads,
          "thread count must be between 1 and " + std::to_string(sphinx::reactor::max_nr_threads));
  require(args.memory_limit % args.nr_threads == 0,
          "memory limit (" + std::to_string(args.memory_limit) +
              ") is not divisible by number of threads (" + std::to_string(args.nr_threads) +
              "), which is required for partitioning");
  auto per_thread_memory = uint64_t(args.memory_limit / args.nr_threads) * 1024 * 1024;
  auto segment_bytes = uint64_t(args.segment_size) * 1024 * 1024;
  require(segment_bytes <= per_thread_memory && per_thread_memory % segment_bytes == 0,
          "per-thread memory must contain whole segments");
}
}  // namespace
Config parse_options(int argc, char* argv[], const std::string& program) {
  static const option long_options[] = {
      {"port", required_argument, nullptr, 'p'},
      {"listen", required_argument, nullptr, 'l'},
      {"memory-limit", required_argument, nullptr, 'm'},
      {"segment-size", required_argument, nullptr, 's'},
      {"listen-backlog", required_argument, nullptr, 'b'},
      {"threads", required_argument, nullptr, 't'},
      {"io-backend", required_argument, nullptr, 'I'},
      {"isolate-cpus", required_argument, nullptr, 'i'},
      {"sched-fifo", no_argument, nullptr, 'S'},
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      {nullptr, 0, nullptr, 0},
  };

  Config args;
  int option;
  int long_index;
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  while ((option = ::getopt_long(argc, argv, "p:l:m:s:b:t:I:i:S", long_options, &long_index)) !=
         -1) {
    switch (option) {
      case 'p':
        args.tcp_port = std::stoi(optarg);
        break;
      case 'l':
        args.listen_addr = optarg;
        break;
      case 'm':
        args.memory_limit = std::stoi(optarg);
        break;
      case 's':
        args.segment_size = std::stoi(optarg);
        break;
      case 'b':
        args.listen_backlog = std::stoi(optarg);
        break;
      case 't':
        args.nr_threads = std::stoi(optarg);
        break;
      case 'I':
        args.backend = optarg;
        break;
      case 'i':
        args.isolate_cpus = parse_cpu_list(optarg);
        break;
      case 'S':
        args.sched_fifo = true;
        break;
      case 'h':
        print_usage(program);
        std::exit(EXIT_SUCCESS);  // NOLINT(concurrency-mt-unsafe)
      case 'v':
        print_version();
        std::exit(EXIT_SUCCESS);  // NOLINT(concurrency-mt-unsafe)
      case '?':
        print_option_error(program, argv[optind - 1], "unrecognized");
        std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
      default:
        print_usage(program);
        std::exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
    }
  }
  validate(args);
  return args;
}
}  // namespace sphinx::server
