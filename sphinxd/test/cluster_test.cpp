// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/cluster.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "MurmurHash3.h"

namespace {

using sphinx::cluster::ConsistentHashRing;
using sphinx::cluster::Node;
using sphinx::cluster::RingEntry;

uint32_t murmur_hash(std::string_view value) {
  uint32_t hash = 0;
  MurmurHash3_x86_32(value.data(), static_cast<int>(value.size()), 1, &hash);
  return hash;
}

TEST(ClusterConfigTest, ParsesSingleAndMultipleNodes) {
  const auto single = sphinx::cluster::parse_nodes("127.0.0.1:11211");
  ASSERT_EQ(single.size(), 1U);
  EXPECT_EQ(single[0].host, "127.0.0.1");
  EXPECT_EQ(single[0].port, 11211);
  EXPECT_EQ(single[0].id(), "127.0.0.1:11211");

  const auto multiple = sphinx::cluster::parse_nodes("cache-a:1,cache-b:65535");
  ASSERT_EQ(multiple.size(), 2U);
  EXPECT_EQ(multiple[0], (Node{"cache-a", 1}));
  EXPECT_EQ(multiple[1], (Node{"cache-b", 65535}));
}

TEST(ClusterConfigTest, RejectsInvalidConfigurations) {
  const std::vector<std::string> invalid = {"",
                                            ",cache-a:1",
                                            "cache-a:1,",
                                            "cache-a",
                                            "cache-a:",
                                            ":1",
                                            "cache-a:0",
                                            "cache-a:65536",
                                            "cache-a:-1",
                                            "cache-a:1.5",
                                            "cache-a:1,cache-a:1",
                                            "cache-a:01,cache-a:1",
                                            "cache a:1",
                                            "cache-a:1, cache-b:2",
                                            "[::1]:1"};
  for (const auto& configuration : invalid) {
    EXPECT_THROW(sphinx::cluster::parse_nodes(configuration), std::invalid_argument)
        << configuration;
  }
}

TEST(ClusterConfigTest, NormalizesPortOnlyForTheNodeId) {
  const auto nodes = sphinx::cluster::parse_nodes("cache-a:00042");
  ASSERT_EQ(nodes.size(), 1U);
  EXPECT_EQ(nodes[0].port, 42);
  EXPECT_EQ(nodes[0].id(), "cache-a:42");
}

TEST(ClusterConfigTest, InputOrderDoesNotChangeTheRing) {
  const ConsistentHashRing first("cache-a:1001,cache-b:1002,cache-c:1003");
  const ConsistentHashRing second("cache-c:1003,cache-a:1001,cache-b:1002");
  ASSERT_EQ(first.entries().size(), 3U * ConsistentHashRing::kVirtualNodesPerNode);
  ASSERT_EQ(first.entries().size(), second.entries().size());
  for (size_t index = 0; index < first.entries().size(); index++) {
    const auto& left = first.entries()[index];
    const auto& right = second.entries()[index];
    EXPECT_EQ(left.hash, right.hash);
    EXPECT_EQ(left.node_id, right.node_id);
    EXPECT_EQ(left.virtual_index, right.virtual_index);
  }
  for (const std::string key : {"a", "b", "fixed-key", "another-key"}) {
    EXPECT_EQ(first.route(key), second.route(key));
  }
}

TEST(ConsistentHashRingTest, BuildsExactly64VirtualNodesPerNodeAndSortsDeterministically) {
  const ConsistentHashRing ring("cache-a:1001,cache-b:1002");
  ASSERT_EQ(ring.entries().size(), 2U * ConsistentHashRing::kVirtualNodesPerNode);
  for (size_t index = 1; index < ring.entries().size(); index++) {
    const RingEntry& previous = ring.entries()[index - 1];
    const RingEntry& current = ring.entries()[index];
    EXPECT_TRUE(std::tie(previous.hash, previous.node_id, previous.virtual_index) <=
                std::tie(current.hash, current.node_id, current.virtual_index));
  }

  for (const auto& node : ring.nodes()) {
    const auto count =
        std::count_if(ring.entries().begin(), ring.entries().end(),
                      [&node](const RingEntry& entry) { return entry.node == node; });
    EXPECT_EQ(count, ConsistentHashRing::kVirtualNodesPerNode);
  }
}

TEST(ConsistentHashRingTest, SingleNodeOwnsEveryKey) {
  const ConsistentHashRing ring("cache-a:1001");
  for (int index = 0; index < 1000; index++) {
    EXPECT_EQ(ring.route("key-" + std::to_string(index)), (Node{"cache-a", 1001}));
  }
}

TEST(ConsistentHashRingTest, ThreeNodesEachReceiveAKey) {
  const ConsistentHashRing ring("cache-a:1001,cache-b:1002,cache-c:1003");
  bool found_a = false;
  bool found_b = false;
  bool found_c = false;
  for (int index = 0; index < 10000; index++) {
    const Node node = ring.route("fixed-key-" + std::to_string(index));
    found_a = found_a || node.host == "cache-a";
    found_b = found_b || node.host == "cache-b";
    found_c = found_c || node.host == "cache-c";
  }
  EXPECT_TRUE(found_a);
  EXPECT_TRUE(found_b);
  EXPECT_TRUE(found_c);
}

TEST(ConsistentHashRingTest, TailWrapsToTheFirstEntry) {
  const ConsistentHashRing ring("cache-a:1001,cache-b:1002");
  ASSERT_FALSE(ring.entries().empty());
  const uint32_t tail_hash = ring.entries().back().hash;
  std::string key;
  for (int index = 0; index < 1000000; index++) {
    const std::string candidate = "wrap-check-" + std::to_string(index);
    if (murmur_hash(candidate) > tail_hash) {
      key = candidate;
      break;
    }
  }
  ASSERT_FALSE(key.empty());
  EXPECT_EQ(ring.route(key), ring.entries().front().node);
}

TEST(ConsistentHashRingTest, EmptyRingReportsAnError) {
  const ConsistentHashRing ring(std::vector<Node>{});
  EXPECT_THROW(ring.route("key"), std::runtime_error);
}

TEST(ConsistentHashRingTest, HashCollisionsHaveStableTieBreaking) {
  std::unordered_map<uint32_t, Node> candidates;
  Node left{"", 0};
  Node right{"", 0};
  uint32_t collision_hash = 0;
  for (uint32_t index = 0; index < 300000; index++) {
    Node candidate{"cache-" + std::to_string(index), static_cast<uint16_t>(1000 + index % 64000)};
    const uint32_t hash = murmur_hash(candidate.id() + "#0");
    const auto [it, inserted] = candidates.emplace(hash, candidate);
    if (!inserted) {
      left = it->second;
      right = candidate;
      collision_hash = hash;
      break;
    }
  }
  ASSERT_NE(left.host, "");
  const ConsistentHashRing ring(std::vector{right, left});
  std::vector<RingEntry> collisions;
  for (const auto& entry : ring.entries()) {
    if (entry.hash == collision_hash && entry.virtual_index == 0) {
      collisions.push_back(entry);
    }
  }
  ASSERT_EQ(collisions.size(), 2U);
  EXPECT_LT(collisions[0].node_id, collisions[1].node_id);
}

TEST(ConsistentHashRingTest, AddingNodeOnlyMovesKeysToTheNewNode) {
  const ConsistentHashRing before("cache-a:1001,cache-b:1002");
  const ConsistentHashRing after("cache-a:1001,cache-b:1002,cache-c:1003");
  for (int index = 0; index < 10000; index++) {
    const std::string key = "migration-key-" + std::to_string(index);
    const Node old_node = before.route(key);
    const Node new_node = after.route(key);
    if (!(old_node == new_node)) {
      EXPECT_EQ(new_node, (Node{"cache-c", 1003})) << key;
    }
  }
}

TEST(ConsistentHashRingTest, RemovingNodeKeepsOtherAssignments) {
  const ConsistentHashRing before("cache-a:1001,cache-b:1002,cache-c:1003");
  const ConsistentHashRing after("cache-a:1001,cache-b:1002");
  for (int index = 0; index < 10000; index++) {
    const std::string key = "migration-key-" + std::to_string(index);
    const Node old_node = before.route(key);
    const Node new_node = after.route(key);
    if (old_node.host != "cache-c") {
      EXPECT_EQ(new_node, old_node) << key;
    }
  }
}

}  // namespace
