// SPDX-License-Identifier: Apache-2.0
#include <sphinx/cluster_client.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: cluster_client_integration_driver host:port,...\n";
    return 2;
  }

  try {
    sphinx::ClusterClient client{std::string_view{argv[1]}};
    auto nodes = sphinx::parse_nodes(argv[1]);
    std::vector<std::string> keys;
    std::unordered_set<std::string> owners;
    for (int index = 0; index < 10000 && owners.size() < nodes.size(); ++index) {
      const auto key = "cluster-key-" + std::to_string(index);
      if (owners.emplace(client.route(key).id()).second) {
        keys.push_back(key);
      }
    }
    if (keys.size() != nodes.size()) {
      throw std::runtime_error{"not every node received a key"};
    }

    for (const auto& key : keys) {
      (void)client.set(key, "value-for-" + key);
    }
    const auto values = client.get_many(keys);
    for (std::size_t index = 0; index < keys.size(); ++index) {
      if (values[index] != "value-for-" + keys[index]) {
        throw std::runtime_error{"multi-get did not return the stored value"};
      }
    }
    std::reverse(nodes.begin(), nodes.end());
    sphinx::ClusterClient reordered{nodes};
    for (const auto& key : keys) {
      const auto owner = client.route(key);
      if (!(owner == reordered.route(key))) {
        throw std::runtime_error{"node input order changed routing"};
      }
      if (client.get(key) != "value-for-" + key) {
        throw std::runtime_error{"get did not return the stored value"};
      }
      for (const auto& node : nodes) {
        sphinx::ClusterClient direct{std::vector<sphinx::Node>{node}};
        const auto value = direct.get(key);
        if (node == owner ? value != "value-for-" + key : value.has_value()) {
          throw std::runtime_error{"key was not isolated to its owner"};
        }
      }
      if (client.remove_status(key) != sphinx::DeleteStatus::Deleted || client.get(key) ||
          client.remove_status(key) != sphinx::DeleteStatus::NotFound) {
        throw std::runtime_error{"delete or subsequent miss returned an invalid result"};
      }
    }

    std::cout << "PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cluster client integration driver: " << error.what() << '\n';
    return 1;
  }
}
