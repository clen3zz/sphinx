// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/spsc_queue.h>

#include <thread>

TEST(QueueTest, try_to_emplace) {
  using namespace sphinx::spsc;
  Queue<int, 128> queue;
  ASSERT_TRUE(queue.empty());
  ASSERT_TRUE(queue.try_to_emplace(1));
  ASSERT_FALSE(queue.empty());
}

TEST(QueueTest, producer_consumer) {
  using namespace sphinx::spsc;
  constexpr int nr_iterations = 1000000;
  Queue<int, 128> queue;
  std::thread producer{[&queue]() {
    for (int i = 0; i < nr_iterations; i++) {
      for (;;) {
        if (queue.try_to_emplace(i)) {
          break;
        }
      }
    }
  }};
  std::thread consumer{[&queue]() {
    for (int i = 0; i < nr_iterations; i++) {
      for (;;) {
        auto* item = queue.front();
        if (item) {
          ASSERT_EQ(i, *item);
          queue.pop();
          break;
        }
      }
    }
  }};
  producer.join();
  consumer.join();
}
