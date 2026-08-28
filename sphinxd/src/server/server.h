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
namespace sphinx::server {

class Server final {
  std::unique_ptr<sphinx::reactor::Reactor> _reactor;
  sphinx::logmem::Log _log;
  std::shared_ptr<sphinx::stats::ServerStats> _stats;

  std::unordered_map<uint64_t, std::shared_ptr<Connection>> _connections;
  uint64_t _next_connection_id = 1;
  std::shared_ptr<std::atomic_bool> _mget_queue_failure_used;

 public:
  Server(const sphinx::logmem::LogConfig& log_config, const std::string& backend, size_t thread_id,
         std::shared_ptr<sphinx::reactor::ReactorGroup> reactor_group,
         std::shared_ptr<sphinx::stats::ServerStats> stats,
         std::shared_ptr<std::atomic_bool> mget_queue_failure_used);
  void serve(const Config& config);

 private:
  void on_message(const sphinx::reactor::MessagePtr& data);
  void handle_command(const Command& command);
  void handle_response(const Response& response);
  void accept(int sockfd);
  void recv(const std::shared_ptr<Connection>& connection,
            const std::shared_ptr<sphinx::reactor::TcpSocket>& socket, std::string_view data);
  size_t process_one(const std::shared_ptr<Connection>& connection, std::string_view data);

  Command make_command(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                       sphinx::memcache::Opcode op, std::string_view key) const;
  void dispatch_command(Command command);
  bool submit_command(size_t target_thread, Command command);
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)：调用处按语义传递线程和连接编号
  void send_response(size_t response_thread, uint64_t connection_id, uint64_t sequence,
                     std::string_view payload, bool multi_get = false, uint32_t key_index = 0);
  void complete_multi_get(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                          uint32_t key_index, std::string_view payload, bool failed = false);
  void enqueue_response(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                        std::string_view payload);
  void close_connection(const std::shared_ptr<Connection>& connection,
                        const std::shared_ptr<sphinx::reactor::TcpSocket>& socket);
  void remove_connection(const std::shared_ptr<Connection>& connection);

  size_t find_target(const sphinx::logmem::Hash& hash) const;
  static uint64_t normalize_expiration(uint64_t expiration);
  bool force_mget_queue_failure_once();
};
}  // namespace sphinx::server
