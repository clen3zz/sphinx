// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/bootstrap/product_config.h>
#include <sphinx/product/http/product_http.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

namespace sphinx {
namespace {

struct WorkerLifetime {
  explicit WorkerLifetime(std::atomic<int>& active) : active{active} { ++active; }
  ~WorkerLifetime() {
    EXPECT_EQ(owner, std::this_thread::get_id());
    --active;
  }
  std::atomic<int>& active;
  std::thread::id owner = std::this_thread::get_id();
};

class EmptyStore final : public ProductStore {
 public:
  std::optional<Product> find(std::uint64_t) override { return std::nullopt; }
  StoreUpdateResult update(const UpdateProductRequest&) override { return {}; }
};
class EmptyCache final : public ProductCache {
 public:
  std::optional<std::string> get(std::string_view) override { return std::nullopt; }
  void put(std::string_view, std::string_view, std::uint32_t) override {}
  void erase(std::string_view) override {}
};

int reserve_port() {
  httplib::Server reservation;
  const int port = reservation.bind_to_any_port("127.0.0.1");
  reservation.stop();
  return port;
}

TEST(ProductHttpServerTest, RejectsAnEmptyWorkerRunner) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  EXPECT_THROW((ProductHttpServer{config, {}, shared}), std::invalid_argument);
}

TEST(ProductHttpServerTest, StopsBeforeServingWithoutCreatingAWorker) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  int created_workers = 0;
  ProductHttpServer server{config,
                           [&](const ProductWorkerLoop&) {
                             ++created_workers;
                             throw std::logic_error{"unexpected worker creation"};
                           },
                           shared};
  server.stop();
  server.stop();
  EXPECT_TRUE(server.serve());
  EXPECT_EQ(created_workers, 0);
  EXPECT_THROW(server.serve(), std::logic_error);
}

TEST(ProductHttpServerTest, RejectsInvalidTransportConfiguration) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  const auto runner = [](const ProductWorkerLoop&) {
    throw std::logic_error{"unexpected worker creation"};
  };
  config.http.worker_count = 65;
  EXPECT_THROW((ProductHttpServer{config, runner, shared}), std::invalid_argument);
  config.http.worker_count = 1;
  config.http.port = 0;
  EXPECT_THROW((ProductHttpServer{config, runner, shared}), std::invalid_argument);
}

TEST(ProductHttpServerTest, JoinsWorkersWhenInitializationFails) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  const int port = reserve_port();
  ASSERT_GT(port, 0);
  config.http.port = static_cast<std::uint16_t>(port);
  config.http.worker_count = 2;
  std::atomic<int> active{0};
  std::atomic<int> created{0};
  ProductHttpServer server{config,
                           [&](const ProductWorkerLoop&) {
                             WorkerLifetime lifetime{active};
                             ++created;
                             throw std::runtime_error{"worker initialization failed"};
                           },
                           shared};
  EXPECT_THROW(server.serve(), std::runtime_error);
  EXPECT_EQ(created.load(), 2);
  EXPECT_EQ(active.load(), 0);
}

TEST(ProductHttpServerTest, OwnsResourcesUntilWorkerExit) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  const int port = reserve_port();
  ASSERT_GT(port, 0);
  config.http.port = static_cast<std::uint16_t>(port);
  config.http.worker_count = 2;
  config.cache.backend = CacheBackend::Redis;
  config.cache_policy.mode = CachePolicyMode::Protected;
  std::atomic<int> active{0};
  ProductHttpServer server{config,
                           [&](const ProductWorkerLoop& run_jobs) {
                             WorkerLifetime lifetime{active};
                             EmptyStore store;
                             EmptyCache cache;
                             ProductService service{store, cache, shared, config.cache_policy};
                             run_jobs(service);
                           },
                           shared};
  bool served = false;
  std::exception_ptr failure;
  std::thread serving{[&] {
    try {
      served = server.serve();
    } catch (...) {
      failure = std::current_exception();
    }
  }};
  httplib::Client client{"127.0.0.1", port};
  client.set_connection_timeout(0, 100000);
  client.set_read_timeout(1, 0);
  bool ready = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (const auto response = client.Get("/metrics")) {
      ready = response->status == 200;
      EXPECT_NE(response->body.find("\"backend\":\"redis\""), std::string::npos);
      EXPECT_NE(response->body.find("\"policy\":\"protected\""), std::string::npos);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  EXPECT_TRUE(ready);
  EXPECT_EQ(active.load(), 2);
  if (ready) {
    const auto response = client.Get("/products/42");
    EXPECT_TRUE(response);
    if (response) {
      EXPECT_EQ(response->status, 404);
    }
  }
  server.stop();
  serving.join();
  EXPECT_FALSE(failure);
  EXPECT_TRUE(served);
  EXPECT_EQ(active.load(), 0);
}

}  // namespace
}  // namespace sphinx
