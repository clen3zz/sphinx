// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product/http/product_http.h>

#include <stdexcept>

namespace sphinx {
namespace {

TEST(ProductHttpServerTest, RejectsAnEmptyServiceFactory) {
  ProductSharedState shared;
  EXPECT_THROW((ProductHttpServer{{}, {}, shared}), std::invalid_argument);
}

TEST(ProductHttpServerTest, StopsBeforeServingWithoutCreatingAWorker) {
  ProductSharedState shared;
  int created_workers = 0;
  ProductHttpServer server{{},
                           [&]() -> ProductService& {
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
  const auto factory = []() -> ProductService& {
    throw std::logic_error{"unexpected worker creation"};
  };
  ProductHttpConfig config;
  config.worker_count = 65;
  EXPECT_THROW((ProductHttpServer{config, factory, shared}), std::invalid_argument);
  config.worker_count = 1;
  config.port = 0;
  EXPECT_THROW((ProductHttpServer{config, factory, shared}), std::invalid_argument);
}

}  // namespace
}  // namespace sphinx
