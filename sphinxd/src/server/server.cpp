// SPDX-License-Identifier: Apache-2.0
#include "server.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

#include "command_executor.h"
namespace sphinx {
namespace {

// 判断是否为无需按键哈希分片的查询类命令（Version、Stats）
bool is_server_info_command(Opcode op) {
  return op == Opcode::Version || op == Opcode::Stats;
}

}  // namespace

// Server 构造函数：初始化 Reactor 驱动、私有日志存储引擎与共享统计指标
Server::Server(const LogConfig& log_config, const std::string& backend, size_t thread_id,
               std::shared_ptr<ReactorGroup> reactor_group, std::shared_ptr<ServerStats> stats,
               std::shared_ptr<std::atomic_bool> mget_queue_failure_used)
    : _reactor{make_reactor(backend, thread_id, std::move(reactor_group),
                            [this](const MessagePtr& data) { on_message(data); })},
      _log{log_config},
      _stats{std::move(stats)},
      _mget_queue_failure_used{std::move(mget_queue_failure_used)} {
}

// 绑定监听地址与端口，启动工作线程的 Reactor 事件循环
void Server::serve(const Config& config) {
  // 1. 创建 TCP 监听器与新连接 accept 回调
  auto accept_fn = [this](int sockfd) { accept(sockfd); };
  auto listener = make_tcp_listener(config.listen_addr, config.tcp_port, config.listen_backlog,
                                    std::move(accept_fn));

  // 2. 注册监听器并进入事件循环
  _reactor->accept(std::move(listener));
  _reactor->run();
}

// Reactor 跨线程消息到达回调
void Server::on_message(const MessagePtr& data) {
  if (data == nullptr) {
    return;
  }

  // 根据消息的实际类型分别分发给命令处理或响应处理
  if (auto command = std::dynamic_pointer_cast<Command>(data)) {
    handle_command(*command);
  } else if (auto response = std::dynamic_pointer_cast<Response>(data)) {
    handle_response(*response);
  }
}

// 执行分配给当前线程的命令并回传响应
void Server::handle_command(const Command& command) {
  auto result = execute_command(_log, *_stats, command);
  send_response(command.source_thread, command.connection_id, command.sequence, result.payload,
                command.multi_get, command.key_index);
}

// 处理其他工作线程返回给本线程托管连接的响应
void Server::handle_response(const Response& response) {
  send_response(_reactor->thread_id(), response.connection_id, response.sequence, response.payload,
                response.multi_get, response.key_index);
}

// 接收并初始化新的客户端连接
void Server::accept(int sockfd) {
  // 1. 分配非零且唯一的连接 ID
  auto connection_id = _next_connection_id++;
  while (connection_id == 0 || _connections.find(connection_id) != _connections.end()) {
    connection_id = _next_connection_id++;
  }

  auto connection = std::make_shared<Connection>(connection_id);

  // 2. 构造套接字对象并绑定数据接收回调
  auto recv_fn = [this, connection](const std::shared_ptr<TcpSocket>& socket,
                                    std::string_view data) { recv(connection, socket, data); };
  auto socket = std::make_shared<TcpSocket>(sockfd, std::move(recv_fn));

  // 3. 关联 Socket、加入连接表并注册到 Reactor 读事件监听
  connection->set_socket(socket);
  _connections.emplace(connection->id(), connection);
  socket->set_tcp_nodelay(true);
  _reactor->recv(std::move(socket));
}

// 客户端套接字数据接收与协议拆包主逻辑
void Server::recv(const std::shared_ptr<Connection>& connection,
                  const std::shared_ptr<TcpSocket>& socket, std::string_view data) {
  // 1. 收到空数据表明对端已关闭连接
  if (data.empty()) {
    close_connection(connection, socket);
    return;
  }

  // 2. 校验接收缓冲区上限（防范畸形请求恶意消耗过多内存）
  constexpr size_t max_request_buffer_size = size_t{8} * 1024 * 1024;
  if (data.size() > max_request_buffer_size - connection->receive_buffer().size()) {
    close_connection(connection, socket);
    return;
  }

  // 3. 将新到达数据追加至接收缓冲区
  connection->receive_buffer().append(data);

  // 4. 循环解析并处理所有完整的命令帧
  while (true) {
    auto view = connection->receive_buffer().string_view();
    auto line_end = view.find('\n');
    if (line_end == std::string_view::npos) {
      return;
    }

    // ASCII 协议规范强制要求以 CRLF 结尾，仅有 LF 时等待更多数据
    if (line_end == 0 || view[line_end - 1] != '\r') {
      return;
    }

    // 尝试解析并执行单条命令
    auto consumed = process_one(connection, view);
    if (consumed == std::numeric_limits<size_t>::max()) {
      close_connection(connection, socket);
      return;
    }

    // 需等待更多包体数据
    if (consumed == 0) {
      return;
    }

    // 从接收缓冲区移除已消费的命令字节
    connection->receive_buffer().remove_prefix(consumed);
  }
}

// 解析并调度单条 Memcached 协议命令
size_t Server::process_one(const std::shared_ptr<Connection>& connection, std::string_view data) {
  Parser parser;
  const auto header_size = parser.parse(data);

  // 1. 处理数值溢出错误
  if (parser.number_overflow()) {
    // 针对 Incr/Decr 算术溢出只消费命令头，允许后续流水线命令继续工作
    const auto& parsed = parser.command();
    const bool arithmetic_overflow = parsed && (std::holds_alternative<IncrCommand>(*parsed) ||
                                                std::holds_alternative<DecrCommand>(*parsed));
    if (arithmetic_overflow) {
      auto sequence = connection->next_request_sequence();
      enqueue_response(connection, sequence, "CLIENT_ERROR invalid numeric argument\r\n");
      return header_size;
    }

    // 存储类字段溢出则关闭连接
    return std::numeric_limits<size_t>::max();
  }

  // 2. 头部不完整，等待后续数据到达
  if (parser.status() == ParseStatus::Incomplete) {
    return 0;
  }

  // 3. 语法非法或无法识别的命令
  if (parser.status() == ParseStatus::Invalid || !parser.command()) {
    auto sequence = connection->next_request_sequence();
    enqueue_response(connection, sequence, "ERROR\r\n");
    return header_size;
  }

  // 4. 分配请求序列号并根据具体命令类型分发处理
  const auto sequence = connection->next_request_sequence();
  RequestProgress progress{header_size};
  const auto& parsed = parser.command().value();

  std::visit(
      [&](const auto& command) {
        using Parsed = std::decay_t<decltype(command)>;

        if constexpr (std::is_same_v<Parsed, SetCommand>) {
          process_storage_command(connection, sequence, data, command.key, command.flags,
                                  command.expiration, command.body, Opcode::Set,
                                  ServerStats::Counter::CmdSet, progress);
        } else if constexpr (std::is_same_v<Parsed, AddCommand>) {
          process_storage_command(connection, sequence, data, command.key, command.flags,
                                  command.expiration, command.body, Opcode::Add,
                                  ServerStats::Counter::CmdAdd, progress);
        } else if constexpr (std::is_same_v<Parsed, ReplaceCommand>) {
          process_storage_command(connection, sequence, data, command.key, command.flags,
                                  command.expiration, command.body, Opcode::Replace,
                                  ServerStats::Counter::CmdReplace, progress);
        } else if constexpr (std::is_same_v<Parsed, GetCommand>) {
          process_get_command(connection, sequence, command);
        } else if constexpr (std::is_same_v<Parsed, DeleteCommand>) {
          _stats->increment(ServerStats::Counter::CmdDelete);
          dispatch_command(make_command(connection, sequence, Opcode::Delete, command.key));
        } else if constexpr (std::is_same_v<Parsed, IncrCommand>) {
          _stats->increment(ServerStats::Counter::CmdIncr);
          auto outgoing = make_command(connection, sequence, Opcode::Incr, command.key);
          outgoing.delta = command.delta;
          dispatch_command(std::move(outgoing));
        } else if constexpr (std::is_same_v<Parsed, DecrCommand>) {
          _stats->increment(ServerStats::Counter::CmdDecr);
          auto outgoing = make_command(connection, sequence, Opcode::Decr, command.key);
          outgoing.delta = command.delta;
          dispatch_command(std::move(outgoing));
        } else if constexpr (std::is_same_v<Parsed, VersionCommand>) {
          dispatch_command(make_command(connection, sequence, Opcode::Version, {}));
        } else if constexpr (std::is_same_v<Parsed, StatsCommand>) {
          dispatch_command(make_command(connection, sequence, Opcode::Stats, {}));
        }
      },
      parsed);

  // 若包体未完整到达，回滚请求序列号并等待更多数据
  if (progress.waiting_for_body) {
    connection->rollback_request_sequence();
    return 0;
  }

  return progress.consumed;
}

void Server::process_get_command(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                                 const GetCommand& command) {
  if (command.keys.empty()) {
    return;
  }

  _stats->increment(ServerStats::Counter::CmdGet);

  if (command.keys.size() == 1) {
    dispatch_command(make_command(connection, sequence, Opcode::Get, command.keys.front()));
    return;
  }

  connection->begin_multi_get(sequence, command.keys.size());
  for (size_t key_index = 0; key_index < command.keys.size(); ++key_index) {
    auto outgoing = make_command(connection, sequence, Opcode::Get, command.keys[key_index]);
    outgoing.multi_get = true;
    outgoing.key_index = static_cast<uint32_t>(key_index);
    dispatch_command(std::move(outgoing));
  }
}

void Server::process_storage_command(const std::shared_ptr<Connection>& connection,
                                     uint64_t sequence, std::string_view data, std::string_view key,
                                     uint64_t flags, uint64_t expiration, const StorageBody& body,
                                     Opcode op, ServerStats::Counter counter,
                                     RequestProgress& progress) {
  const auto frame_size = body.frame_size();
  if (!frame_size || !body.available(data)) {
    progress.waiting_for_body = true;
    return;
  }

  progress.consumed = body.offset + *frame_size;
  if (!body.has_valid_terminator(data)) {
    enqueue_response(connection, sequence, "CLIENT_ERROR bad data chunk\r\n");
    return;
  }

  if (key.size() > std::numeric_limits<uint32_t>::max() ||
      flags > std::numeric_limits<uint32_t>::max() ||
      expiration > std::numeric_limits<uint32_t>::max()) {
    enqueue_response(connection, sequence, "CLIENT_ERROR invalid numeric argument\r\n");
    return;
  }

  auto value = data.substr(body.offset, static_cast<size_t>(body.size));
  auto outgoing = make_command(connection, sequence, op, key);
  outgoing.blob.assign(value);
  outgoing.flags = static_cast<uint32_t>(flags);
  outgoing.expiration = normalize_expiration(expiration);

  _stats->increment(counter);
  dispatch_command(std::move(outgoing));
}

// 构造 Command 对象
Command Server::make_command(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                             Opcode op, std::string_view key) const {
  return {connection->id(), sequence, _reactor->thread_id(), op, std::string{key}};
}

// 调度命令：路由给本线程或跨线程投递至目标工作线程
void Server::dispatch_command(Command command) {
  // 1. 验证目标连接是否仍然存活
  auto connection_it = _connections.find(command.connection_id);
  if (connection_it == _connections.end()) {
    return;
  }
  const auto& connection = connection_it->second;

  // 2. 服务器查询命令留在本线程处理，数据命令根据键哈希计算目标分区线程
  const auto target = is_server_info_command(command.op)
                          ? _reactor->thread_id()
                          : find_target(Object::hash_of(command.key));

  // 3. 目标即本线程，直接同步调用 handle_command
  if (target == _reactor->thread_id()) {
    handle_command(command);
    return;
  }

  // 4. 目标为远端工作线程，通过 Reactor 跨线程消息队列投递
  const auto sequence = command.sequence;
  const auto key_index = command.key_index;
  const auto multi_get = command.multi_get;

  if (!submit_command(target, std::move(command))) {
    // 队列已满投递失败时的容错处理
    if (multi_get) {
      complete_multi_get(connection, sequence, key_index, {}, true);
    } else {
      enqueue_response(connection, sequence, "SERVER_ERROR request queue is full\r\n");
    }
  }
}

// 向目标工作线程投递 Command 消息
bool Server::submit_command(size_t target_thread, Command command) {
  // 支持测试环境下的 multi-get 队列满故障模拟
  if (command.multi_get && force_mget_queue_failure_once()) {
    return false;
  }

  auto message = std::make_shared<Command>(std::move(command));
  return _reactor->send_msg(target_thread, message);
}

// 将执行结果作为 Response 投递回连接所在的主调线程
void Server::send_response(size_t response_thread, uint64_t connection_id, uint64_t sequence,
                           std::string_view payload, bool multi_get, uint32_t key_index) {
  // 1. 目标为主线程自身，直接入队或聚合
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

  // 2. 目标为其他线程，构造跨线程 Response 消息
  auto message = std::make_shared<Response>(connection_id, sequence, std::string{payload},
                                            multi_get, key_index);

  // 优先采用延迟投递，避免数据线程阻塞等待连接线程
  if (_reactor->send_msg_deferred(response_thread, message)) {
    return;
  }

  // 回退至有界同步队列再次尝试
  if (_reactor->send_msg(response_thread, message)) {
    return;
  }

  // 双重投递皆失败属于不可恢复的致命状态，终止进程以防止响应滞留死锁
  std::cerr << "fatal: response delivery failed for connection " << connection_id << '\n'
            << std::flush;
  std::terminate();
}

// 记录 multi-get 聚合结果，所有分片就绪后写出完整响应
void Server::complete_multi_get(const std::shared_ptr<Connection>& connection, uint64_t sequence,
                                uint32_t key_index, std::string_view payload, bool failed) {
  auto response = connection->add_multi_get_piece(sequence, key_index, payload, failed);
  if (response) {
    enqueue_response(connection, sequence, response.value());
  }
}

// 将响应按 sequence 入队并写出，处理可能的套接字关闭
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

// 关闭客户端连接并从 Reactor 注销
void Server::close_connection(const std::shared_ptr<Connection>& connection,
                              const std::shared_ptr<TcpSocket>& socket) {
  if (connection->closed()) {
    return;
  }

  connection->mark_closed();
  remove_connection(connection);
  _reactor->close(socket);
}

// 从当前线程的连接表中移除指定连接
void Server::remove_connection(const std::shared_ptr<Connection>& connection) {
  auto it = _connections.find(connection->id());
  if (it != _connections.end() && it->second == connection) {
    _connections.erase(it);
  }
}

// 将 Memcached 协议格式的相对过期时间转换为绝对秒级时间戳
uint64_t Server::normalize_expiration(uint64_t expiration) {
  if (expiration == 0) {
    return 0;
  }

  // 依据协议规范，30 天以内的值作为相对当前时间的秒数，超过则视为绝对 UNIX 时间戳
  constexpr uint64_t thirty_days = uint64_t{60} * 60 * 24 * 30;
  using namespace std::chrono;
  auto now =
      static_cast<uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());

  if (expiration <= thirty_days) {
    return now + expiration;
  }

  return expiration;
}

// 根据键的哈希值对线程数取模，计算分片目标工作线程 ID
size_t Server::find_target(const Hash& hash) const {
  auto nr_threads = _reactor->nr_threads();
  if (nr_threads == 1) {
    return _reactor->thread_id();
  }

  return hash % nr_threads;
}

// 模拟 multi-get 队列投递失败的测试桩（由 SPHINXD_TEST_FAIL_MGET_QUEUE_ONCE 触发）
bool Server::force_mget_queue_failure_once() const {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (std::getenv("SPHINXD_TEST_FAIL_MGET_QUEUE_ONCE") == nullptr || !_mget_queue_failure_used) {
    return false;
  }

  bool expected = false;
  return _mget_queue_failure_used->compare_exchange_strong(expected, true,
                                                           std::memory_order_relaxed);
}

}  // namespace sphinx
