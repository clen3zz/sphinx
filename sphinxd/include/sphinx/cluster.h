/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sphinx::cluster {

struct Node
{
  std::string host;
  uint16_t port;

  std::string id() const;
  bool operator==(const Node& other) const;
};

std::vector<Node>
parse_nodes(std::string_view specification);

struct RingEntry
{
  uint32_t hash;
  std::string node_id;
  uint32_t virtual_index;
  Node node;
};

class ConsistentHashRing
{
public:
  static constexpr uint32_t kVirtualNodesPerNode = 64;

  explicit ConsistentHashRing(std::vector<Node> nodes);
  explicit ConsistentHashRing(std::string_view specification);

  Node route(std::string_view key) const;

  const std::vector<Node>& nodes() const;
  const std::vector<RingEntry>& entries() const;

private:
  std::vector<Node> nodes_;
  std::vector<RingEntry> entries_;
};

} // namespace sphinx::cluster
