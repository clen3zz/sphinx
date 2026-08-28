// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sphinx {

struct Node {
  std::string host;
  uint16_t port;

  std::string id() const;
  bool operator==(const Node& other) const;
};

std::vector<Node> parse_nodes(std::string_view specification);

struct RingEntry {
  uint32_t hash;
  std::string node_id;
  uint32_t virtual_index;
  Node node;
};

class ConsistentHashRing {
 public:
  static constexpr uint32_t kVirtualNodesPerNode = 64;

  explicit ConsistentHashRing(std::vector<Node> nodes);
  explicit ConsistentHashRing(std::string_view specification);

  Node route(std::string_view key) const;

  const std::vector<Node>& nodes() const;
  const std::vector<RingEntry>& entries() const;

 private:
  std::vector<Node> _nodes;
  std::vector<RingEntry> _entries;
};

}  // namespace sphinx
