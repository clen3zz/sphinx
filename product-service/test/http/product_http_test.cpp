// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/backends/mysql/mysql_runtime.h>
#include <sphinx/product/bootstrap/product_config.h>
#include <sphinx/product/http/product_http.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

namespace sphinx {
namespace {

int reserve_port() {
  httplib::Server reservation;
  const int port = reservation.bind_to_any_port("127.0.0.1");
  reservation.stop();
  return port;
}

TEST(ProductHttpServerTest, StopsBeforeServingWithoutCreatingAWorker) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  ProductHttpServer server{config, shared};
  server.stop();
  server.stop();
  EXPECT_TRUE(server.serve());
  EXPECT_THROW(server.serve(), std::logic_error);
}

TEST(ProductHttpServerTest, RejectsInvalidTransportConfiguration) {
  ProductSharedState shared;
  ProductRuntimeConfig config;
  config.http.worker_count = 65;
  EXPECT_THROW((ProductHttpServer{config, shared}), std::invalid_argument);
  config.http.worker_count = 1;
  config.http.port = 0;
  EXPECT_THROW((ProductHttpServer{config, shared}), std::invalid_argument);
}

TEST(ProductHttpServerTest, JoinsWorkersWhenInitializationFails) {
  MySqlRuntime runtime;
  ProductSharedState shared;
  ProductRuntimeConfig config;
  const int port = reserve_port();
  ASSERT_GT(port, 0);
  config.http.port = static_cast<std::uint16_t>(port);
  config.http.worker_count = 2;
  ProductHttpServer server{config, shared};
  // 空数据库配置使工作线程初始化失败；runtime 析构前所有线程 guard 必须已释放。
  EXPECT_THROW(server.serve(), std::invalid_argument);
}

TEST(ProductHttpServerTest, OwnsResourcesUntilWorkerExit) {
  MySqlRuntime runtime;
  ProductSharedState shared;
  ProductRuntimeConfig config;
  const int port = reserve_port();
  ASSERT_GT(port, 0);
  config.http.port = static_cast<std::uint16_t>(port);
  config.http.worker_count = 2;
  config.cache.backend = CacheBackend::Redis;
  config.cache_policy.mode = CachePolicyMode::Protected;
  config.mysql.user = "test";
  config.mysql.database = "sphinx_test";
  const int unavailable_port = reserve_port();
  ASSERT_GT(unavailable_port, 0);
  config.mysql.port = static_cast<std::uint16_t>(unavailable_port);
  ProductHttpServer server{config, shared};
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
  if (ready) {
    const auto response = client.Get("/products/42?fresh=1");
    EXPECT_TRUE(response);
    if (response) {
      EXPECT_EQ(response->status, 503);
    }
  }
  server.stop();
  serving.join();
  EXPECT_FALSE(failure);
  EXPECT_TRUE(served);
}

}  // namespace
}  // namespace sphinx
