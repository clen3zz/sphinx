// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/product_read_coordinator.h>

#include <array>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

TEST(ProductReadCoordinatorTest, SharesFlightAndPublishesLeaderResult) {
  ProductReadCoordinator coordinator{ProductReadOptions{}};
  auto leader_tickets = coordinator.acquire_many({42});
  auto follower_tickets = coordinator.acquire_many({42});
  ASSERT_EQ(leader_tickets.front().role(), ReadRole::Leader);
  ASSERT_EQ(follower_tickets.front().role(), ReadRole::Follower);

  leader_tickets.front().complete({ProductStatus::Ok, Product{42, "tea", 199, 1}});
  const auto result = follower_tickets.front().wait_until(std::chrono::steady_clock::now() +
                                                          std::chrono::milliseconds{50});

  EXPECT_EQ(result.status, ProductStatus::Ok);
  EXPECT_EQ(result.product.value_or(Product{}).id, 42U);
  EXPECT_EQ(coordinator.active_key_count(), 0U);
}

TEST(ProductReadCoordinatorTest, KeepsExistingFollowersWhenCapacityIsFull) {
  ProductReadOptions options;
  options.max_inflight_keys = 1;
  ProductReadCoordinator coordinator{options};
  auto leader = coordinator.acquire_many({1});
  auto follower = coordinator.acquire_many({1});
  auto rejected = coordinator.acquire_many({2});

  EXPECT_EQ(leader.front().role(), ReadRole::Leader);
  EXPECT_EQ(follower.front().role(), ReadRole::Follower);
  EXPECT_EQ(rejected.front().role(), ReadRole::Rejected);
  EXPECT_EQ(rejected.front().wait_until(std::chrono::steady_clock::now()).status,
            ProductStatus::ReadBusy);
  EXPECT_EQ(coordinator.active_key_count(), 1U);

  leader.front().complete({ProductStatus::NotFound, std::nullopt});
  EXPECT_EQ(follower.front()
                .wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{50})
                .status,
            ProductStatus::NotFound);
  EXPECT_EQ(coordinator.active_key_count(), 0U);
}

TEST(ProductReadCoordinatorTest, BoundsLoadPermitsAndMoveTransfersReleaseResponsibility) {
  ProductReadOptions options;
  options.max_concurrent_loads = 1;
  ProductReadCoordinator coordinator{options};
  auto first = coordinator.try_acquire_load();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(coordinator.active_load_count(), 1U);
  EXPECT_FALSE(coordinator.try_acquire_load().has_value());

  auto moved = std::move(first);
  first.reset();
  EXPECT_EQ(coordinator.active_load_count(), 1U);
  moved.reset();
  EXPECT_EQ(coordinator.active_load_count(), 0U);
}

TEST(ProductReadCoordinatorTest, FollowerTimeoutDoesNotCancelLeader) {
  ProductReadCoordinator coordinator{ProductReadOptions{}};
  auto leader = coordinator.acquire_many({7});
  auto follower = coordinator.acquire_many({7});

  const auto timed_out = follower.front().wait_until(std::chrono::steady_clock::now());

  EXPECT_EQ(timed_out.status, ProductStatus::ReadBusy);
  EXPECT_EQ(coordinator.active_key_count(), 1U);
  leader.front().complete({ProductStatus::NotFound, std::nullopt});
  EXPECT_EQ(coordinator.active_key_count(), 0U);
}

TEST(ProductReadCoordinatorTest, AbandonedLeaderPublishesErrorAndRemovesFlight) {
  ProductReadCoordinator coordinator{ProductReadOptions{}};
  std::optional<ProductReadTicket> leader;
  {
    auto tickets = coordinator.acquire_many({9});
    leader.emplace(std::move(tickets.front()));
  }
  auto followers = coordinator.acquire_many({9});
  ASSERT_EQ(followers.front().role(), ReadRole::Follower);

  leader.reset();

  const auto result = followers.front().wait_until(std::chrono::steady_clock::now() +
                                                   std::chrono::milliseconds{50});
  EXPECT_EQ(result.status, ProductStatus::InternalError);
  EXPECT_EQ(coordinator.active_key_count(), 0U);
}

TEST(ProductReadCoordinatorTest, CrossedBatchesCanPublishOwnLeadersBeforeWaiting) {
  ProductReadCoordinator coordinator{ProductReadOptions{}};
  auto first = coordinator.acquire_many({1, 2});
  auto second = coordinator.acquire_many({2, 1});

  ASSERT_EQ(first[0].role(), ReadRole::Leader);
  ASSERT_EQ(first[1].role(), ReadRole::Leader);
  ASSERT_EQ(second[0].role(), ReadRole::Follower);
  ASSERT_EQ(second[1].role(), ReadRole::Follower);
  first[0].complete({ProductStatus::NotFound, std::nullopt});
  first[1].complete({ProductStatus::Ok, Product{2, "tea", 199, 1}});

  EXPECT_EQ(
      second[0].wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{50}).status,
      ProductStatus::Ok);
  EXPECT_EQ(
      second[1].wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{50}).status,
      ProductStatus::NotFound);
}

TEST(ProductReadCoordinatorTest, ConcurrentRequestsCreateExactlyOneLeader) {
  constexpr std::size_t request_count = 8;
  ProductReadCoordinator coordinator{ProductReadOptions{}};
  std::array<ProductReadTicket, request_count> tickets;
  std::array<std::thread, request_count> threads;

  for (std::size_t index = 0; index < request_count; ++index) {
    threads[index] = std::thread{[&coordinator, &tickets, index] {
      auto acquired = coordinator.acquire_many({42});
      tickets[index] = std::move(acquired.front());
    }};
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::size_t leader_count = 0;
  std::size_t leader_index = 0;
  for (std::size_t index = 0; index < request_count; ++index) {
    if (tickets[index].role() == ReadRole::Leader) {
      ++leader_count;
      leader_index = index;
    } else {
      EXPECT_EQ(tickets[index].role(), ReadRole::Follower);
    }
  }
  ASSERT_EQ(leader_count, 1U);
  tickets[leader_index].complete({ProductStatus::Ok, Product{42, "tea", 199, 1}});
  for (auto& ticket : tickets) {
    EXPECT_EQ(
        ticket.wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{50}).status,
        ProductStatus::Ok);
  }
  EXPECT_EQ(coordinator.active_key_count(), 0U);
}

TEST(ProductReadCoordinatorTest, RejectsInvalidOptionsAndZeroIDs) {
  ProductReadOptions options;
  options.max_inflight_keys = 0;
  EXPECT_THROW((ProductReadCoordinator{options}), std::invalid_argument);

  options = ProductReadOptions{};
  options.wait_timeout = std::chrono::milliseconds{0};
  EXPECT_THROW((ProductReadCoordinator{options}), std::invalid_argument);

  ProductReadCoordinator coordinator{ProductReadOptions{}};
  EXPECT_THROW(coordinator.acquire_many({0}), std::invalid_argument);
}

}  // namespace
}  // namespace sphinx
