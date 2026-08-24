#include <sphinx/cluster_client.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

int
main(int argc, char* argv[])
{
  if (argc != 2) {
    std::cerr << "usage: cluster_client_integration_driver host:port\n";
    return 2;
  }

  try {
    sphinx::cluster::ClusterClient client{std::string_view{argv[1]}};
    constexpr std::string_view key = "cluster-client-integration-key";
    constexpr std::string_view value = "cluster-client-integration-value";

    if (!client.set(key, value)) {
      throw std::runtime_error{"set did not report success"};
    }
    const auto hit = client.get(key);
    if (!hit || *hit != value) {
      throw std::runtime_error{"get did not return the stored value"};
    }
    if (client.remove_status(key) != sphinx::cluster::DeleteStatus::Deleted) {
      throw std::runtime_error{"delete did not report DELETED"};
    }
    if (client.get(key).has_value()) {
      throw std::runtime_error{"get returned a value after delete"};
    }

    std::cout << "PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cluster client integration driver: " << error.what() << '\n';
    return 1;
  }
}
