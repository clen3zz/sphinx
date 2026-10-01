// SPDX-License-Identifier: Apache-2.0
#include <httplib.h>
#include <sphinx/product/backends/redis/redis_product_cache.h>
#include <sphinx/product/backends/sphinx/sphinx_product_cache.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

struct Options {
  std::string group;
  std::string backend;
  std::string scenario;
  std::uint16_t port;
  std::size_t concurrency;
  double seconds;
  double warmup_seconds;
  std::size_t keys;
  std::size_t value_bytes;
};

struct StartGate {
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t ready = 0;
  bool started = false;
  Clock::time_point start;
  Clock::time_point deadline;
};

struct WorkerResult {
  std::uint64_t completed = 0;
  std::uint64_t errors = 0;
  std::uint64_t warmup_errors = 0;
  std::string first_error;
  std::vector<double> latencies_us;
  Clock::time_point finish;
};

struct Request {
  std::string path;
  std::vector<std::string> keys;
  std::vector<sphinx::CacheWriteEntry> writes;
};

std::vector<Request> make_requests(const Options& options) {
  const bool batch = options.scenario == "batch_get" || options.scenario == "batch_put";
  const auto width = batch ? 32U : 1U;
  const std::string value(options.value_bytes, 'x');
  std::vector<Request> requests;
  requests.reserve(options.keys);
  for (std::size_t index = 0; index < options.keys; ++index) {
    Request request;
    request.path = batch ? "/products?ids=" : "/products/";
    for (std::size_t item = 0; item < width; ++item) {
      const auto id = (index + item) % options.keys + 1;
      if (options.group == "direct") {
        request.keys.push_back("benchmark:" + std::to_string(id));
        if (options.scenario == "batch_put") {
          request.writes.push_back({request.keys.back(), value, 300});
        }
      }
      if (item != 0) {
        request.path += ',';
      }
      request.path += std::to_string(id);
    }
    if (options.scenario == "fresh_get") {
      request.path += "?fresh=1";
    }
    requests.push_back(std::move(request));
  }
  return requests;
}

std::unique_ptr<sphinx::ProductCache> make_cache(const Options& options) {
  if (options.backend == "sphinx") {
    return std::make_unique<sphinx::SphinxProductCache>("127.0.0.1:" + std::to_string(options.port),
                                                        std::chrono::seconds{2});
  }
  sphinx::RedisOptions redis;
  redis.port = options.port;
  redis.connect_timeout = std::chrono::seconds{2};
  redis.io_timeout = std::chrono::seconds{2};
  return std::make_unique<sphinx::RedisProductCache>(redis);
}

// Each worker owns its client; setup and request construction stay outside measured operations.
class Workload final {
 public:
  Workload(const Options& options, const std::vector<Request>& requests, std::size_t worker)
      : _options{options},
        _requests{requests},
        _next{worker * 131U},
        _value(options.value_bytes, 'x') {
    const bool batch = options.scenario == "batch_get" || options.scenario == "batch_put";
    if (options.group == "http") {
      _http = std::make_unique<httplib::Client>("127.0.0.1", options.port);
      // The fixed-worker server holds a worker while a keep-alive socket waits for a request.
      // Short HTTP connections let concurrency exceed the worker count without idle-socket bias.
      _http->set_keep_alive(false);
      _http->set_tcp_nodelay(true);
      _http->set_connection_timeout(2, 0);
      _http->set_read_timeout(5, 0);
      // Validate payload shape once before timing; measured requests still check status and source.
      const auto response = _http->Get(_requests.front().path);
      if (!response || response->status != 200) {
        throw std::runtime_error{"HTTP preflight failed"};
      }
      const auto body = Json::parse(response->body);
      if (batch) {
        if (!body.contains("items") || body["items"].size() != 32) {
          throw std::runtime_error{"HTTP batch preflight shape mismatch"};
        }
        std::uint64_t expected_id = 1;
        for (const auto& item : body["items"]) {
          if (item.contains("error") || item.value("id", 0ULL) != expected_id ||
              !item.contains("product") || item["product"].value("id", 0ULL) != expected_id ||
              item["product"].value("version", 0ULL) != 1) {
            throw std::runtime_error{"HTTP batch preflight payload mismatch"};
          }
          ++expected_id;
        }
      } else if (body.value("id", 0ULL) != 1 || body.value("version", 0ULL) != 1) {
        throw std::runtime_error{"HTTP product preflight shape mismatch"};
      }
    } else {
      _cache = make_cache(options);
    }
  }

  void execute() {
    const auto& request = _requests[_next++ % _requests.size()];
    if (_http) {
      const auto response = _http->Get(request.path);
      const auto expected_source = _options.scenario == "fresh_get" ? "BYPASS" : "HIT";
      if (!response || response->status != 200 || response->body.empty() ||
          response->get_header_value("X-Cache") != expected_source) {
        throw std::runtime_error{"HTTP status, body or cache-source mismatch"};
      }
    } else if (_options.scenario == "get") {
      const auto value = _cache->get(request.keys.front());
      if (!value || *value != _value) {
        throw std::runtime_error{"direct GET payload mismatch"};
      }
    } else if (_options.scenario == "put") {
      _cache->put(request.keys.front(), _value, 300);
    } else if (_options.scenario == "batch_get") {
      const auto values = _cache->get_many(request.keys);
      if (values.size() != request.keys.size() ||
          std::any_of(values.begin(), values.end(),
                      [this](const auto& value) { return !value || *value != _value; })) {
        throw std::runtime_error{"direct multi-get payload mismatch"};
      }
    } else {
      _cache->put_many(request.writes);
    }
  }

 private:
  const Options& _options;
  const std::vector<Request>& _requests;
  std::size_t _next;
  std::string _value;
  std::unique_ptr<httplib::Client> _http;
  std::unique_ptr<sphinx::ProductCache> _cache;
};

bool execute_checked(Workload& workload, WorkerResult& result) {
  try {
    workload.execute();
    return true;
  } catch (const std::exception& error) {
    if (result.first_error.empty()) {
      result.first_error = error.what();
    }
    return false;
  }
}

void run_worker(const Options& options, const std::vector<Request>& requests, std::size_t worker,
                StartGate& gate, WorkerResult& result) {
  std::unique_ptr<Workload> workload;
  try {
    workload = std::make_unique<Workload>(options, requests, worker);
    result.latencies_us.reserve(200000);
    const auto warmup_deadline =
        Clock::now() + std::chrono::duration<double>{options.warmup_seconds};
    while (Clock::now() < warmup_deadline) {
      if (!execute_checked(*workload, result)) {
        ++result.warmup_errors;
      }
    }
  } catch (const std::exception& error) {
    result.first_error = error.what();
    ++result.warmup_errors;
  }
  {
    std::unique_lock lock{gate.mutex};
    ++gate.ready;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] { return gate.started; });
  }
  while (workload && Clock::now() < gate.deadline) {
    const auto begin = Clock::now();
    if (execute_checked(*workload, result)) {
      ++result.completed;
    } else {
      ++result.errors;
    }
    const auto elapsed = std::chrono::duration<double, std::micro>{Clock::now() - begin};
    result.latencies_us.push_back(elapsed.count());
  }
  result.finish = Clock::now();
}

double percentile(const std::vector<double>& samples, std::size_t percent) {
  if (samples.empty()) {
    return 0;
  }
  const auto index = (samples.size() * percent + 99) / 100 - 1;
  return samples[index];
}

Options parse_options(int argc, char* argv[]) {
  if (argc != 10) {
    throw std::invalid_argument{
        "usage: benchmark http|direct sphinx|redis scenario port concurrency seconds "
        "warmup_seconds keys value_bytes"};
  }
  const auto port = std::stoul(argv[4]);
  Options options{argv[1],
                  argv[2],
                  argv[3],
                  static_cast<std::uint16_t>(port),
                  std::stoul(argv[5]),
                  std::stod(argv[6]),
                  std::stod(argv[7]),
                  std::stoul(argv[8]),
                  std::stoul(argv[9])};
  const bool http = options.group == "http";
  const bool valid_scenario =
      http ? (options.scenario == "get" || options.scenario == "batch_get" ||
              options.scenario == "fresh_get")
           : (options.scenario == "get" || options.scenario == "put" ||
              options.scenario == "batch_get" || options.scenario == "batch_put");
  if ((!http && options.group != "direct") ||
      (options.backend != "sphinx" && options.backend != "redis") || !valid_scenario || port == 0 ||
      port > 65535 || options.concurrency == 0 || options.concurrency > 256 ||
      !std::isfinite(options.seconds) || options.seconds <= 0 || options.seconds > 60 ||
      !std::isfinite(options.warmup_seconds) || options.warmup_seconds < 0 ||
      options.warmup_seconds > 60 || options.keys < 32 || options.keys > 65536 ||
      options.value_bytes == 0 || options.value_bytes > 4096) {
    throw std::invalid_argument{"invalid benchmark options"};
  }
  return options;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    const auto requests = make_requests(options);
    StartGate gate;
    std::vector<WorkerResult> results(options.concurrency);
    std::vector<std::thread> threads;
    threads.reserve(options.concurrency);
    for (std::size_t worker = 0; worker < options.concurrency; ++worker) {
      threads.emplace_back(run_worker, std::cref(options), std::cref(requests), worker,
                           std::ref(gate), std::ref(results[worker]));
    }
    {
      std::unique_lock lock{gate.mutex};
      gate.changed.wait(lock, [&] { return gate.ready == options.concurrency; });
    }
    // The runner snapshots backend CPU after warmup, then releases the measured phase.
    std::cout << "READY\n" << std::flush;
    std::string start;
    std::getline(std::cin, start);
    const auto cpu_start = std::clock();
    {
      std::lock_guard lock{gate.mutex};
      gate.start = Clock::now();
      gate.deadline = gate.start + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<double>{options.seconds});
      gate.started = true;
    }
    gate.changed.notify_all();
    for (auto& thread : threads) {
      thread.join();
    }
    const auto cpu_seconds = static_cast<double>(std::clock() - cpu_start) / CLOCKS_PER_SEC;
    std::uint64_t completed = 0;
    std::uint64_t errors = 0;
    std::uint64_t warmup_errors = 0;
    auto finish = gate.start;
    std::vector<double> latencies;
    std::vector<std::string> first_errors;
    for (const auto& result : results) {
      completed += result.completed;
      errors += result.errors;
      warmup_errors += result.warmup_errors;
      finish = std::max(finish, result.finish);
      latencies.insert(latencies.end(), result.latencies_us.begin(), result.latencies_us.end());
      if (!result.first_error.empty()) {
        first_errors.push_back(result.first_error);
      }
    }
    std::sort(latencies.begin(), latencies.end());
    const auto elapsed = std::chrono::duration<double>{finish - gate.start}.count();
    const auto width = options.scenario == "batch_get" || options.scenario == "batch_put" ? 32 : 1;
    const Json report{{"completed", completed},
                      {"errors", errors},
                      {"warmup_errors", warmup_errors},
                      {"first_errors", first_errors},
                      {"elapsed_seconds", elapsed},
                      {"operations_per_second", static_cast<double>(completed) / elapsed},
                      {"items_per_second", static_cast<double>(completed) * width / elapsed},
                      {"client_cpu_seconds", cpu_seconds},
                      {"latency_us",
                       {{"p50", percentile(latencies, 50)},
                        {"p95", percentile(latencies, 95)},
                        {"p99", percentile(latencies, 99)}}}};
    std::cout << report.dump() << '\n';
    return errors == 0 && warmup_errors == 0 && completed != 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
