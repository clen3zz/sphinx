#include <sphinx/cluster_client.h>

#include <iostream>
#include <string>
#include <string_view>

namespace {

void
print_usage(std::ostream& output)
{
  output << "Usage:\n"
         << "  sphinx-cluster --nodes <host:port,...> route <key>\n"
         << "  sphinx-cluster --nodes <host:port,...> set <key> <value>\n"
         << "  sphinx-cluster --nodes <host:port,...> get <key>\n"
         << "  sphinx-cluster --nodes <host:port,...> delete <key>\n";
}

int
fail_usage(const std::string& message)
{
  std::cerr << "sphinx-cluster: " << message << "\n";
  print_usage(std::cerr);
  return 2;
}

} // namespace

int
main(int argc, char* argv[])
{
  if (argc == 2 && std::string_view{argv[1]} == "--help") {
    print_usage(std::cout);
    return 0;
  }
  if (argc < 4 || std::string_view{argv[1]} != "--nodes") {
    return fail_usage("expected --nodes <list> <command>");
  }

  const std::string_view node_spec{argv[2]};
  const std::string_view command{argv[3]};
  try {
    sphinx::cluster::ClusterClient client{node_spec};
    if (command == "route") {
      if (argc != 5) {
        return fail_usage("route expects one key");
      }
      std::cout << client.route(argv[4]).id() << '\n';
      return 0;
    }
    if (command == "set") {
      if (argc != 6) {
        return fail_usage("set expects a key and a value");
      }
      client.set(argv[4], argv[5]);
      std::cout << "STORED\n";
      return 0;
    }
    if (command == "get") {
      if (argc != 5) {
        return fail_usage("get expects one key");
      }
      const auto value = client.get(argv[4]);
      if (!value) {
        std::cout << "NOT_FOUND\n";
      } else {
        std::cout << *value << '\n';
      }
      return 0;
    }
    if (command == "delete") {
      if (argc != 5) {
        return fail_usage("delete expects one key");
      }
      const auto status = client.remove_status(argv[4]);
      std::cout << (status == sphinx::cluster::DeleteStatus::Deleted ? "DELETED" : "NOT_FOUND")
                << '\n';
      return 0;
    }
    return fail_usage("unknown command '" + std::string{command} + "'");
  } catch (const std::exception& error) {
    std::cerr << "sphinx-cluster: " << error.what() << '\n';
    return 1;
  }
}
