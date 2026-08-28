// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/logmem.h>
#include <sphinx/reactor.h>
#include <sphinx/stats.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "config.h"
#include "connection.h"
#include "message.h"
namespace sphinx {

// 单工作线程服务实例类：
// 封装当前线程的私有 Reactor 事件驱动循环、日志内存存储引擎（Log）、客户端连接表与跨线程消息路由
class Server final {
  struct RequestProgress {
    size_t consumed = 0;
    bool waiting_for_body = false;
  };

  std::unique_ptr<Reactor> _reactor;    // 线程私有 Reactor 实例
  Log _log;                             // 线程私有日志结构内存存储引擎
  std::shared_ptr<ServerStats> _stats;  // 全局原子统计指标共享指针

  std::unordered_map<uint64_t, std::shared_ptr<Connection>> _connections;  // 本线程管理的活跃连接表
  uint64_t _next_connection_id = 1;                                        // 连接递增 ID 生成器
  std::shared_ptr<std::atomic_bool> _mget_queue_failure_used;              // 故障模拟/测试标记

 public:
  Server(const LogConfig& log_config, const std::string& backend, size_t thread_id,
         std::shared_ptr<ReactorGroup> reactor_group, std::shared_ptr<ServerStats> stats,
         std::shared_ptr<std::atomic_bool> mget_queue_failure_used);

  // 绑定监听套接字并启动 Reactor 事件驱动循环
  void serve(const Config& config);

 private:
  // Reactor 邮箱消息就绪回调（跨线程消息分发）
  void on_message(const MessagePtr& data);

  // 处理分发给本线程执行的存储/查询命令
  void handle_command(const Command& command);

  // 处理其他线程回传给本线程连接的响应结果
  void handle_response(const Response& response);

  // 接收新建立的 TCP 客户端连接
  void accept(int sockfd);

  // 处理客户端套接字接收到的数据
  void recv(const std::shared_ptr<Connection>& connection, const std::shared_ptr<TcpSocket>& socket,
            std::string_view data);

  // 从连接接收缓冲区中解析并执行单个协议请求
  size_t process_one(const std::shared_ptr<Connection>& connection, std::string_view data);

  // Set / Add / Replace 共享相同的数据帧校验和内部命令构造流程
  void process_storage_command(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                               std::string_view data, std::string_view key, uint64_t flags,
                               uint64_t expiration, const StorageBody& body, Opcode op,
                               ServerStats::Counter counter, RequestProgress& progress);

  // 处理单键与 multi-get 查询
  void process_get_command(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                           const GetCommand& command);

  // 构造标准命令对象
  Command make_command(const std::shared_ptr<Connection>& connection, uint64_t sequence, Opcode op,
                       std::string_view key) const;

  // 根据键的一致性哈希计算目标线程并分发命令
  void dispatch_command(Command command);

  // 向指定目标线程的 Reactor 邮箱提交命令
  bool submit_command(size_t target_thread, Command command);

  // 将执行结果作为响应回传给发起请求的来源线程
  void send_response(size_t response_thread, uint64_t connection_id, uint64_t sequence,
                     std::string_view payload, bool multi_get = false, uint32_t key_index = 0);

  // 处理 multi-get 子分片聚合与最终写出
  void complete_multi_get(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                          uint32_t key_index, std::string_view payload, bool failed = false);

  // 将响应入队并按序列号顺序写回客户端
  void enqueue_response(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                        std::string_view payload);

  // 关闭指定客户端套接字并释放资源
  void close_connection(const std::shared_ptr<Connection>& connection,
                        const std::shared_ptr<TcpSocket>& socket);

  // 从本地连接表中移除已关闭的连接
  void remove_connection(const std::shared_ptr<Connection>& connection);

  // 基于哈希值计算分片路由的目标线程 ID
  size_t find_target(const Hash& hash) const;

  // 将相对过期时间戳标准化为绝对时间
  static uint64_t normalize_expiration(uint64_t expiration);

  // 故障注入辅助函数（仅在测试多键失败分支时使用）
  bool force_mget_queue_failure_once() const;
};

}  // namespace sphinx
