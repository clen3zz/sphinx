// SPDX-License-Identifier: Apache-2.0
#include "server.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "command_executor.h"
namespace sphinx::server {
namespace {
bool is_control_command(sphinx::memcache::Opcode op) {
  using sphinx::memcache::Opcode;
  return op == Opcode::Version || op == Opcode::Stats;
}
}  // namespace

Server::Server(const sphinx::logmem::LogConfig& log_config, const std::string& backend,
               size_t thread_id, std::shared_ptr<sphinx::reactor::ReactorGroup> reactor_group,
               std::shared_ptr<sphinx::stats::ServerStats> stats,
               std::shared_ptr<std::atomic_bool> mget_queue_failure_used)
    : _reactor{sphinx::reactor::make_reactor(
          backend, thread_id, std::move(reactor_group),
          [this](const sphinx::reactor::MessagePtr& data) { on_message(data); })},
      _log{log_config},
      _stats{std::move(stats)},
      _mget_queue_failure_used{std::move(mget_queue_failure_used)} {
}

void Server::serve(const Config& config) {
  auto accept_fn = [this](int sockfd) { accept(sockfd); };
  auto listener = sphinx::reactor::make_tcp_listener(config.listen_addr, config.tcp_port,
                                                     config.listen_backlog, std::move(accept_fn));
  _reactor->accept(std::move(listener));
  _reactor->run();
}

void Server::on_message(const sphinx::reactor::MessagePtr& data) {
  if (data == nullptr) {
    return;
  }
  auto message = std::dynamic_pointer_cast<Message>(data);
  if (!message) {
    return;
  }
  if (auto* command = std::get_if<Command>(&message->payload)) {
    handle_command(*command);
  } else if (auto* response = std::get_if<Response>(&message->payload)) {
    handle_response(*response);
  }
}

void Server::handle_command(const Command& command) {
  auto result = execute_command(_log, *_stats, command);
  send_response(command.source_thread, command.connection_id, command.sequence, result.payload,
                command.multi_get, command.key_index);
}

void Server::handle_response(const Response& response) {
  send_response(_reactor->thread_id(), response.connection_id, response.sequence, response.payload,
                response.multi_get, response.key_index);
}

void Server::accept(int sockfd) {
  auto connection_id = _next_connection_id++;
  while (connection_id == 0 || _connections.find(connection_id) != _connections.end()) {
    connection_id = _next_connection_id++;
  }
  auto connection = std::make_shared<Connection>(connection_id);
  auto recv_fn = [this, connection](const std::shared_ptr<sphinx::reactor::TcpSocket>& socket,
                                    std::string_view data) { recv(connection, socket, data); };
  auto socket = std::make_shared<sphinx::reactor::TcpSocket>(sockfd, std::move(recv_fn));
  connection->set_socket(socket);
  _connections.emplace(connection->id(), connection);
  socket->set_tcp_nodelay(true);
  _reactor->recv(std::move(socket));
}

void Server::recv(const std::shared_ptr<Connection>& connection,
                  const std::shared_ptr<sphinx::reactor::TcpSocket>& socket,
                  std::string_view data) {
  if (data.empty()) {
    close_connection(connection, socket);
    return;
  }
  constexpr size_t max_request_buffer_size = size_t{8} * 1024 * 1024;
  if (data.size() > max_request_buffer_size - connection->receive_buffer().size()) {
    close_connection(connection, socket);
    return;
  }
  connection->receive_buffer().append(data);
  for (;;) {
    auto view = connection->receive_buffer().string_view();
    auto line_end = view.find('\n');
    if (line_end == std::string_view::npos) {
      return;
    }
    // ASCII 命令只能由 CRLF 结束。若只有 LF，则将其留在接收缓冲区中，
    // 避免误消费后续流水线命令的字节。
    if (line_end == 0 || view[line_end - 1] != '\r') {
      return;
    }
    auto consumed = process_one(connection, view);
    if (consumed == std::numeric_limits<size_t>::max()) {
      close_connection(connection, socket);
      return;
    }
    if (consumed == 0) {
      return;
    }
    connection->receive_buffer().remove_prefix(consumed);
  }
}

size_t Server::process_one(const std::shared_ptr<Connection>& connection, std::string_view data) {
  using namespace sphinx::memcache;
  Parser parser;
  const auto header_size = parser.parse(data);
  if (parser.number_overflow()) {
    // 无法放入 uint64_t 的增量属于命令错误。只消费该命令头，使后续流水线
    // 请求仍可用；存储字段溢出则保留原有的关闭连接行为。
    const auto& parsed = parser.command();
    const bool arithmetic_overflow = parsed && (std::holds_alternative<IncrCommand>(*parsed) ||
                                                std::holds_alternative<DecrCommand>(*parsed));
    if (arithmetic_overflow) {
      auto sequence = connection->next_request_sequence();
      enqueue_response(connection, sequence, "CLIENT_ERROR invalid numeric argument\r\n");
      return header_size;
    }
    return std::numeric_limits<size_t>::max();
  }
  if (parser.status() == ParseStatus::Incomplete) {
    return 0;
  }
  if (parser.status() == ParseStatus::Invalid || !parser.command()) {
    auto sequence = connection->next_request_sequence();
    enqueue_response(connection, sequence, "ERROR\r\n");
    return header_size;
  }

  const auto sequence = connection->next_request_sequence();
  size_t consumed = header_size;
  bool waiting_for_body = false;
  const auto& parsed = parser.command().value();
  std::visit(
      [&](const auto& command) {
        using Parsed = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Parsed, SetCommand> || std::is_same_v<Parsed, AddCommand> ||
                      std::is_same_v<Parsed, ReplaceCommand>) {
          const auto frame_size = command.body.frame_size();
          if (!frame_size || !command.body.available(data)) {
            waiting_for_body = true;
            return;
          }
          consumed = command.body.offset + *frame_size;
          if (!command.body.has_valid_terminator(data)) {
            enqueue_response(connection, sequence, "CLIENT_ERROR bad data chunk\r\n");
            return;
          }
          constexpr auto op = []() {
            if constexpr (std::is_same_v<Parsed, SetCommand>) {
              return Opcode::Set;
            } else if constexpr (std::is_same_v<Parsed, AddCommand>) {
              return Opcode::Add;
            } else {
              return Opcode::Replace;
            }
          }();
          if (command.key.size() > std::numeric_limits<uint32_t>::max() ||
              command.flags > std::numeric_limits<uint32_t>::max() ||
              command.expiration > std::numeric_limits<uint32_t>::max()) {
            enqueue_response(connection, sequence, "CLIENT_ERROR invalid numeric argument\r\n");
            return;
          }
          auto value = data.substr(command.body.offset, static_cast<size_t>(command.body.size));
          auto outgoing = make_command(connection, sequence, op, command.key);
          outgoing.blob.assign(value);
          outgoing.flags = static_cast<uint32_t>(command.flags);
          outgoing.expiration = normalize_expiration(command.expiration);
          if constexpr (std::is_same_v<Parsed, SetCommand>) {
            _stats->increment(sphinx::stats::ServerStats::Counter::CmdSet);
          } else if constexpr (std::is_same_v<Parsed, AddCommand>) {
            _stats->increment(sphinx::stats::ServerStats::Counter::CmdAdd);
          } else {
            _stats->increment(sphinx::stats::ServerStats::Counter::CmdReplace);
          }
          dispatch_command(std::move(outgoing));
        } else if constexpr (std::is_same_v<Parsed, GetCommand>) {
          if (command.keys.empty()) {
            return;
          }
          _stats->increment(sphinx::stats::ServerStats::Counter::CmdGet);
          if (command.keys.size() == 1) {
            dispatch_command(make_command(connection, sequence, Opcode::Get, command.keys.front()));
            return;
          }

          // 在各键所属的工作线程读取数据，最后按顺序发出一个 END 响应。
          connection->begin_multi_get(sequence, command.keys.size());
          for (size_t key_index = 0; key_index < command.keys.size(); ++key_index) {
            auto outgoing =
                make_command(connection, sequence, Opcode::Get, command.keys[key_index]);
            outgoing.multi_get = true;
            outgoing.key_index = static_cast<uint32_t>(key_index);
            dispatch_command(std::move(outgoing));
          }
        } else if constexpr (std::is_same_v<Parsed, DeleteCommand>) {
          _stats->increment(sphinx::stats::ServerStats::Counter::CmdDelete);
          dispatch_command(make_command(connection, sequence, Opcode::Delete, command.key));
        } else if constexpr (std::is_same_v<Parsed, IncrCommand> ||
                             std::is_same_v<Parsed, DecrCommand>) {
          const auto op = std::is_same_v<Parsed, IncrCommand> ? Opcode::Incr : Opcode::Decr;
          _stats->increment(op == Opcode::Incr ? sphinx::stats::ServerStats::Counter::CmdIncr
                                               : sphinx::stats::ServerStats::Counter::CmdDecr);
          auto outgoing = make_command(connection, sequence, op, command.key);
          outgoing.delta = command.delta;
          dispatch_command(std::move(outgoing));
        } else if constexpr (std::is_same_v<Parsed, VersionCommand>) {
          dispatch_command(make_command(connection, sequence, Opcode::Version, {}));
        } else if constexpr (std::is_same_v<Parsed, StatsCommand>) {
          dispatch_command(make_command(connection, sequence, Opcode::Stats, {}));
        }
      },
      parsed);
  if (waiting_for_body) {
    connection->rollback_request_sequence();
    return 0;
  }
  return consumed;
}

Command Server::make_command(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                             sphinx::memcache::Opcode op, std::string_view key) const {
  return {connection->id(), sequence, _reactor->thread_id(), op, std::string{key}};
}

void Server::dispatch_command(Command command) {
  auto connection_it = _connections.find(command.connection_id);
  if (connection_it == _connections.end()) {
    return;
  }
  const auto& connection = connection_it->second;
  const auto target = is_control_command(command.op)
                          ? _reactor->thread_id()
                          : find_target(sphinx::logmem::Object::hash_of(command.key));
  if (target == _reactor->thread_id()) {
    handle_command(command);
    return;
  }
  const auto sequence = command.sequence;
  const auto key_index = command.key_index;
  const auto multi_get = command.multi_get;
  if (!submit_command(target, std::move(command))) {
    if (multi_get) {
      complete_multi_get(connection, sequence, key_index, {}, true);
    } else {
      enqueue_response(connection, sequence, "SERVER_ERROR request queue is full\r\n");
    }
  }
}

bool Server::submit_command(size_t target_thread, Command command) {
  if (command.multi_get && force_mget_queue_failure_once()) {
    return false;
  }
  auto message = std::make_shared<Message>(std::move(command));
  return _reactor->send_msg(target_thread,
                            std::static_pointer_cast<sphinx::reactor::Message>(message));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)：调用处按语义传递线程和连接编号
void Server::send_response(size_t response_thread, uint64_t connection_id, uint64_t sequence,
                           std::string_view payload, bool multi_get, uint32_t key_index) {
  if (response_thread == _reactor->thread_id()) {
    auto it = _connections.find(connection_id);
    if (it != _connections.end()) {
      if (multi_get) {
        complete_multi_get(it->second, sequence, key_index, payload);
      } else {
        enqueue_response(it->second, sequence, payload);
      }
    }
    return;
  }
  auto message = std::static_pointer_cast<sphinx::reactor::Message>(std::make_shared<Message>(
      Response{connection_id, sequence, std::string{payload}, multi_get, key_index}));
  // 延迟投递可避免数据工作线程等待连接工作线程。
  if (_reactor->send_msg_deferred(response_thread, message)) {
    return;
  }
  // 再尝试一次有界队列；若两种路径都失败，则终止进程，避免连接的有序响应队列
  // 永久滞留。
  if (_reactor->send_msg(response_thread, message)) {
    return;
  }
  std::cerr << "fatal: response delivery failed for connection " << connection_id << '\n'
            << std::flush;
  std::terminate();
}

void Server::complete_multi_get(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                                uint32_t key_index, std::string_view payload, bool failed) {
  auto response = connection->add_multi_get_piece(sequence, key_index, payload, failed);
  if (response) {
    enqueue_response(connection, sequence, response.value());
  }
}

void Server::enqueue_response(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                              std::string_view payload) {
  auto status = connection->enqueue_response(sequence, payload, *_reactor);
  if (status == Connection::WriteStatus::SocketClosed) {
    auto socket = connection->socket();
    if (socket) {
      close_connection(connection, socket);
    } else {
      remove_connection(connection);
    }
  } else if (status == Connection::WriteStatus::SocketUnavailable) {
    remove_connection(connection);
  }
}

void Server::close_connection(const std::shared_ptr<Connection>& connection,
                              const std::shared_ptr<sphinx::reactor::TcpSocket>& socket) {
  if (connection->closed()) {
    return;
  }
  connection->mark_closed();
  remove_connection(connection);
  _reactor->close(socket);
}

void Server::remove_connection(const std::shared_ptr<Connection>& connection) {
  auto it = _connections.find(connection->id());
  if (it != _connections.end() && it->second == connection) {
    _connections.erase(it);
  }
}

uint64_t Server::normalize_expiration(uint64_t expiration) {
  if (expiration == 0) {
    return 0;
  }
  constexpr uint64_t thirty_days = uint64_t{60} * 60 * 24 * 30;
  using namespace std::chrono;
  auto now = uint64_t(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
  if (expiration <= thirty_days) {
    return now + expiration;
  }
  return expiration;
}

size_t Server::find_target(const sphinx::logmem::Hash& hash) const {
  auto nr_threads = _reactor->nr_threads();
  if (nr_threads == 1) {
    return _reactor->thread_id();
  }
  return hash % nr_threads;
}

bool Server::force_mget_queue_failure_once() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (std::getenv("SPHINXD_TEST_FAIL_MGET_QUEUE_ONCE") == nullptr || !_mget_queue_failure_used) {
    return false;
  }
  bool expected = false;
  return _mget_queue_failure_used->compare_exchange_strong(expected, true,
                                                           std::memory_order_relaxed);
}
}  // namespace sphinx::server
