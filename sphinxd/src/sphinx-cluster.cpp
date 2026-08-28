// SPDX-License-Identifier: Apache-2.0
#include <sphinx/cluster_client.h>

#include <iostream>
#include <string>
#include <string_view>

using namespace sphinx;

namespace {

// 打印命令行工具使用帮助说明
void print_usage(std::ostream& output) {
  output << "Usage:\n"
         << "  sphinx-cluster --nodes <host:port,...> route <key>\n"
         << "  sphinx-cluster --nodes <host:port,...> set <key> <value>\n"
         << "  sphinx-cluster --nodes <host:port,...> get <key>\n"
         << "  sphinx-cluster --nodes <host:port,...> delete <key>\n";
}

// 打印用法错误信息与帮助说明，返回参数错误状态码
int fail_usage(const std::string& message) {
  std::cerr << "sphinx-cluster: " << message << "\n";
  print_usage(std::cerr);
  return 2;
}

}  // namespace

// sphinx-cluster 命令行客户端入口
int main(int argc, char* argv[]) {
  // 1. 帮助选项快速响应 (--help)
  if (argc == 2 && std::string_view{argv[1]} == "--help") {
    print_usage(std::cout);
    return 0;
  }

  // 2. 命令行参数基本校验（必须指定 --nodes <节点列表> 以及后续子命令）
  if (argc < 4 || std::string_view{argv[1]} != "--nodes") {
    return fail_usage("expected --nodes <list> <command>");
  }

  const std::string_view node_spec{argv[2]};
  const std::string_view command{argv[3]};

  try {
    // 3. 初始化集群客户端实例（解析节点拓扑并构建哈希环）
    ClusterClient client{node_spec};

    // 4. 根据子命令分发执行相应逻辑
    // 子命令：route —— 查询键对应路由的目标节点
    if (command == "route") {
      if (argc != 5) {
        return fail_usage("route expects one key");
      }

      std::cout << client.route(argv[4]).id() << '\n';
      return 0;
    }

    // 子命令：set —— 存储键值对
    if (command == "set") {
      if (argc != 6) {
        return fail_usage("set expects a key and a value");
      }

      client.set(argv[4], argv[5]);
      std::cout << "STORED\n";
      return 0;
    }

    // 子命令：get —— 获取键对应的值
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

    // 子命令：delete —— 删除指定的键
    if (command == "delete") {
      if (argc != 5) {
        return fail_usage("delete expects one key");
      }

      const auto status = client.remove_status(argv[4]);
      std::cout << (status == DeleteStatus::Deleted ? "DELETED" : "NOT_FOUND") << '\n';
      return 0;
    }

    // 未知命令错误处理
    return fail_usage("unknown command '" + std::string{command} + "'");
  } catch (const std::exception& error) {
    // 5. 顶层异常捕获
    std::cerr << "sphinx-cluster: " << error.what() << '\n';
    return 1;
  }
}
