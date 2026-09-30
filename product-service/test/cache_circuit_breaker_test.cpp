// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <sphinx/cache_circuit_breaker.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sphinx {
namespace {

class ManualCacheClock final {
 public:
  std::chrono::steady_clock::time_point now() const noexcept {
    std::lock_guard lock{_mutex};
    return _now;
  }

  void advance(std::chrono::milliseconds amount) noexcept {
    std::lock_guard lock{_mutex};
    _now += amount;
  }

 private:
  mutable std::mutex _mutex;
  std::chrono::steady_clock::time_point _now{};
};

CacheBreakerOptions test_options() { return {2, std::chrono::milliseconds{100}}; }

TEST(CacheCircuitBreakerTest, OpensAfterThresholdAndReportsRemainingDelay) {
  ManualCacheClock clock;
  CacheCircuitBreaker breaker{test_options(), [&clock] { return clock.now(); }};

  auto first_result = breaker.try_acquire();
  ASSERT_TRUE(first_result.has_value());
  auto first = std::move(first_result).value_or(CacheOperationPermit{});
  first.fail();
  EXPECT_EQ(breaker.snapshot().state, CacheBreakerState::Closed);
  auto second_result = breaker.try_acquire();
  ASSERT_TRUE(second_result.has_value());
  auto second = std::move(second_result).value_or(CacheOperationPermit{});
  second.fail();

  const auto open = breaker.snapshot();
  EXPECT_EQ(open.state, CacheBreakerState::Open);
  EXPECT_EQ(open.consecutive_failures, 2U);
  EXPECT_EQ(open.retry_after, std::chrono::milliseconds{100});
  EXPECT_FALSE(breaker.try_acquire().has_value());

  clock.advance(std::chrono::milliseconds{40});
  EXPECT_EQ(breaker.snapshot().retry_after, std::chrono::milliseconds{60});
}

TEST(CacheCircuitBreakerTest, SuccessfulClosedOperationResetsConsecutiveFailures) {
  ManualCacheClock clock;
  CacheCircuitBreaker breaker{test_options(), [&clock] { return clock.now(); }};
  auto first_result = breaker.try_acquire();
  ASSERT_TRUE(first_result.has_value());
  auto first = std::move(first_result).value_or(CacheOperationPermit{});
  first.fail();
  auto success_result = breaker.try_acquire();
  ASSERT_TRUE(success_result.has_value());
  auto success = std::move(success_result).value_or(CacheOperationPermit{});
  success.succeed();
  EXPECT_EQ(breaker.snapshot().consecutive_failures, 0U);
}

TEST(CacheCircuitBreakerTest, AllowsOnlyOneHalfOpenProbeAndClosesOnSuccess) {
  ManualCacheClock clock;
  CacheCircuitBreaker breaker{test_options(), [&clock] { return clock.now(); }};
  for (std::size_t index = 0; index < 2; ++index) {
    auto permit_result = breaker.try_acquire();
    ASSERT_TRUE(permit_result.has_value());
    auto permit = std::move(permit_result).value_or(CacheOperationPermit{});
    permit.fail();
  }
  clock.advance(std::chrono::milliseconds{100});
  auto probe_result = breaker.try_acquire();
  ASSERT_TRUE(probe_result.has_value());
  auto probe = std::move(probe_result).value_or(CacheOperationPermit{});
  EXPECT_EQ(breaker.snapshot().state, CacheBreakerState::HalfOpen);
  EXPECT_FALSE(breaker.try_acquire().has_value());
  probe.succeed();

  const auto closed = breaker.snapshot();
  EXPECT_EQ(closed.state, CacheBreakerState::Closed);
  EXPECT_EQ(closed.consecutive_failures, 0U);
  auto next = breaker.try_acquire();
  EXPECT_TRUE(next.has_value());
}

TEST(CacheCircuitBreakerTest, FailedProbeReopensForAnotherInterval) {
  ManualCacheClock clock;
  CacheCircuitBreaker breaker{test_options(), [&clock] { return clock.now(); }};
  for (std::size_t index = 0; index < 2; ++index) {
    auto permit_result = breaker.try_acquire();
    ASSERT_TRUE(permit_result.has_value());
    auto permit = std::move(permit_result).value_or(CacheOperationPermit{});
    permit.fail();
  }
  clock.advance(std::chrono::milliseconds{100});
  auto probe_result = breaker.try_acquire();
  ASSERT_TRUE(probe_result.has_value());
  auto probe = std::move(probe_result).value_or(CacheOperationPermit{});
  probe.fail();

  EXPECT_EQ(breaker.snapshot().state, CacheBreakerState::Open);
  EXPECT_EQ(breaker.snapshot().retry_after, std::chrono::milliseconds{100});
}

TEST(CacheCircuitBreakerTest, IgnoresResultsFromAnOlderGeneration) {
  ManualCacheClock clock;
  CacheCircuitBreaker breaker{test_options(), [&clock] { return clock.now(); }};
  auto delayed_result = breaker.try_acquire();
  ASSERT_TRUE(delayed_result.has_value());
  auto delayed = std::move(delayed_result).value_or(CacheOperationPermit{});
  for (std::size_t index = 0; index < 2; ++index) {
    auto failure_result = breaker.try_acquire();
    ASSERT_TRUE(failure_result.has_value());
    auto failure = std::move(failure_result).value_or(CacheOperationPermit{});
    failure.fail();
  }
  EXPECT_EQ(breaker.snapshot().state, CacheBreakerState::Open);
  delayed.succeed();
  EXPECT_EQ(breaker.snapshot().state, CacheBreakerState::Open);
}

TEST(CacheCircuitBreakerTest, MovingOrAbandoningPermitReportsExactlyOneFailure) {
  ManualCacheClock clock;
  CacheCircuitBreaker breaker{test_options(), [&clock] { return clock.now(); }};
  {
    auto original = breaker.try_acquire();
    ASSERT_TRUE(original.has_value());
    auto moved = std::move(original);
    original.reset();
    moved.reset();
  }
  EXPECT_EQ(breaker.snapshot().consecutive_failures, 1U);
}

TEST(CacheCircuitBreakerTest, RejectsInvalidOptions) {
  CacheBreakerOptions options;
  options.failure_threshold = 0;
  EXPECT_THROW((CacheCircuitBreaker{options}), std::invalid_argument);
  options = test_options();
  options.open_interval = std::chrono::milliseconds{0};
  EXPECT_THROW((CacheCircuitBreaker{options}), std::invalid_argument);
}

}  // namespace
}  // namespace sphinx
