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

#include <sphinx/buffer.h>
#include <sphinx/hardware.h>
#include <sphinx/logmem.h>
#include <sphinx/memory.h>
#include <sphinx/protocol.h>
#include <sphinx/reactor.h>
#include <sphinx/string.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <getopt.h>
#include <libgen.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/mman.h>

#include "version.h"

static std::string program;

static constexpr int DEFAULT_TCP_PORT = 11211;
static constexpr int DEFAULT_UDP_PORT = 0; /* disabled */
static constexpr const char* DEFAULT_LISTEN_ADDR = "0.0.0.0";
static constexpr int DEFAULT_MEMORY_LIMIT = 64;
static constexpr int DEFAULT_SEGMENT_SIZE = 2;
static constexpr int DEFAULT_LISTEN_BACKLOG = 1024;
static constexpr int DEFAULT_NR_THREADS = 4;

struct Args
{
  std::string listen_addr = DEFAULT_LISTEN_ADDR;
  int tcp_port = DEFAULT_TCP_PORT;
  int memory_limit = DEFAULT_MEMORY_LIMIT; /* in MB */
  int segment_size = DEFAULT_SEGMENT_SIZE; /* in MB */
  int listen_backlog = DEFAULT_LISTEN_BACKLOG;
  int nr_threads = DEFAULT_NR_THREADS;
  std::string backend = sphinx::reactor::Reactor::default_backend();
  std::set<int> isolate_cpus;
  bool sched_fifo = false;
};

enum class MessageType : uint8_t
{
  Command,
  Response,
};

struct Message
{
  MessageType type = MessageType::Command;
  uint64_t connection_id;
  uint64_t sequence;
  uint8_t source_thread;
  sphinx::buffer::Buffer buffer;
  sphinx::memcache::Opcode op;
  uint32_t key_size;
  uint32_t flags;
  uint64_t expiration;
  std::string payload;

  std::string_view key() const
  {
    return buffer.string_view().substr(0, key_size);
  }
  std::string_view blob() const
  {
    return buffer.string_view().substr(key_size);
  }
};

struct Connection
{
  uint64_t id;
  sphinx::buffer::Buffer _rx_buffer;
  uint64_t next_request_sequence = 0;
  uint64_t next_response_sequence = 0;
  std::map<uint64_t, std::string> pending_responses;
  std::weak_ptr<sphinx::reactor::TcpSocket> socket;
  bool closed = false;
};

class Server
{
  std::unique_ptr<sphinx::reactor::Reactor> _reactor;
  sphinx::logmem::Log _log;

public:
  Server(const sphinx::logmem::LogConfig& log_cfg,
         const std::string& backend,
         size_t thread_id,
         size_t nr_threads);
  void serve(const Args& args);

private:
  void on_message(void* data);
  void accept(int sockfd);
  void recv(const std::shared_ptr<Connection>& conn,
            std::shared_ptr<sphinx::reactor::TcpSocket> sock,
            std::string_view msg);
  size_t process_one(const std::shared_ptr<Connection>& conn, std::string_view msg);
  void cmd_set(size_t response_thread,
               uint64_t connection_id,
               uint64_t sequence,
               std::string_view key,
               std::string_view blob,
               uint32_t flags,
               uint64_t expiration);
  void cmd_add(size_t response_thread,
               uint64_t connection_id,
               uint64_t sequence,
               std::string_view key,
               std::string_view blob,
               uint32_t flags,
               uint64_t expiration);
  void cmd_replace(size_t response_thread,
                   uint64_t connection_id,
                   uint64_t sequence,
                   std::string_view key,
                   std::string_view blob,
                   uint32_t flags,
                   uint64_t expiration);
  void cmd_get(size_t response_thread,
               uint64_t connection_id,
               uint64_t sequence,
               std::string_view key);
  void send_response(size_t response_thread,
                     uint64_t connection_id,
                     uint64_t sequence,
                     std::string_view msg);
  void enqueue_response(const std::shared_ptr<Connection>& conn,
                        uint64_t sequence,
                        std::string_view msg);
  void close_connection(const std::shared_ptr<Connection>& conn,
                        const std::shared_ptr<sphinx::reactor::TcpSocket>& sock);
  size_t find_target(const sphinx::logmem::Hash& hash) const;
  static uint64_t normalize_expiration(uint64_t expiration);

  std::unordered_map<uint64_t, std::shared_ptr<Connection>> _connections;
  uint64_t _next_connection_id = 1;
};

Server::Server(const sphinx::logmem::LogConfig& log_cfg,
               const std::string& backend,
               size_t thread_id,
               size_t nr_threads)
  : _reactor{sphinx::reactor::make_reactor(backend,
                                           thread_id,
                                           nr_threads,
                                           [this](void* data) { this->on_message(data); })}
  , _log{log_cfg}
{
}

void
Server::serve(const Args& args)
{
  auto accept_fn = [this](int sockfd) { this->accept(sockfd); };
  auto listener = sphinx::reactor::make_tcp_listener(
    args.listen_addr, args.tcp_port, args.listen_backlog, std::move(accept_fn));
  _reactor->accept(std::move(listener));
  _reactor->run();
}

void
Server::on_message(void* data)
{
  using namespace sphinx::memcache;
  if (data == nullptr) {
    return;
  }
  auto* message = static_cast<Message*>(data);
  auto type = message->type;
  if (type == MessageType::Response) {
    auto it = _connections.find(message->connection_id);
    if (it != _connections.end()) {
      enqueue_response(it->second, message->sequence, message->payload);
    }
    delete message;
    return;
  }
  if (type != MessageType::Command) {
    delete message;
    return;
  }
  auto* cmd = message;
  switch (cmd->op) {
    case Opcode::Version:
      send_response(cmd->source_thread, cmd->connection_id, cmd->sequence, "VERSION 1.5.16\r\n");
      break;
    case Opcode::Set: {
      cmd_set(cmd->source_thread,
              cmd->connection_id,
              cmd->sequence,
              cmd->key(),
              cmd->blob(),
              cmd->flags,
              cmd->expiration);
      break;
    }
    case Opcode::Add: {
      cmd_add(cmd->source_thread,
              cmd->connection_id,
              cmd->sequence,
              cmd->key(),
              cmd->blob(),
              cmd->flags,
              cmd->expiration);
      break;
    }
    case Opcode::Replace: {
      cmd_replace(cmd->source_thread,
                  cmd->connection_id,
                  cmd->sequence,
                  cmd->key(),
                  cmd->blob(),
                  cmd->flags,
                  cmd->expiration);
      break;
    }
    case Opcode::Get: {
      cmd_get(cmd->source_thread, cmd->connection_id, cmd->sequence, cmd->key());
      break;
    }
  }
  delete cmd;
}

void
Server::accept(int sockfd)
{
  auto conn = std::make_shared<Connection>();
  do {
    conn->id = _next_connection_id++;
    if (conn->id == 0) {
      conn->id = _next_connection_id++;
    }
  } while (_connections.find(conn->id) != _connections.end());
  auto recv_fn = [this, conn](const std::shared_ptr<sphinx::reactor::TcpSocket>& sock,
                              std::string_view msg) { this->recv(conn, sock, msg); };
  auto sock = std::make_shared<sphinx::reactor::TcpSocket>(sockfd, std::move(recv_fn));
  conn->socket = sock;
  _connections.emplace(conn->id, conn);
  sock->set_tcp_nodelay(true);
  this->_reactor->recv(std::move(sock));
}

void
Server::recv(const std::shared_ptr<Connection>& conn,
             std::shared_ptr<sphinx::reactor::TcpSocket> sock,
             std::string_view msg)
{
  if (msg.size() == 0) {
    close_connection(conn, sock);
    return;
  }
  constexpr size_t max_request_buffer_size = 8 * 1024 * 1024;
  if (msg.size() > max_request_buffer_size - conn->_rx_buffer.size()) {
    close_connection(conn, sock);
    return;
  }
  conn->_rx_buffer.append(msg);
  for (;;) {
    auto view = conn->_rx_buffer.string_view();
    if (view.find('\n') == std::string_view::npos) {
      return;
    }
    size_t nr_consumed = process_one(conn, view);
    if (nr_consumed == std::numeric_limits<size_t>::max()) {
      close_connection(conn, sock);
      return;
    }
    if (!nr_consumed) {
      return;
    }
    conn->_rx_buffer.remove_prefix(nr_consumed);
  }
}

size_t
Server::process_one(const std::shared_ptr<Connection>& conn, std::string_view msg)
{
  using namespace sphinx::memcache;
  Parser parser;
  size_t nr_consumed = parser.parse(msg);
  if (parser._number_overflow) {
    return std::numeric_limits<size_t>::max();
  }
  if (!parser._op) {
    auto line_end = msg.find('\n');
    if (line_end == std::string_view::npos) {
      return 0;
    }
    auto sequence = conn->next_request_sequence++;
    enqueue_response(conn, sequence, "ERROR\r\n");
    return line_end + 1;
  }
  if (nr_consumed > msg.size()) {
    return 0;
  }
  const auto op = parser._op.value();
  auto sequence = conn->next_request_sequence++;
  switch (op) {
    case Opcode::Version: {
      send_response(_reactor->thread_id(), conn->id, sequence, "VERSION 1.5.16\r\n");
      return nr_consumed;
    }
    case Opcode::Set:
    case Opcode::Add:
    case Opcode::Replace: {
      if (parser._blob_size > std::numeric_limits<size_t>::max() - 2 || nr_consumed > msg.size() ||
          parser._blob_size + 2 > msg.size() - nr_consumed) {
        conn->next_request_sequence--;
        return 0;
      }
      size_t data_block_size = size_t(parser._blob_size) + 2;
      auto consumed = nr_consumed + data_block_size;
      const auto& key = parser.key();
      if (key.size() > std::numeric_limits<uint32_t>::max() ||
          parser._flags > std::numeric_limits<uint32_t>::max() ||
          parser._expiration > std::numeric_limits<uint32_t>::max()) {
        enqueue_response(conn, sequence, "CLIENT_ERROR invalid numeric argument\r\n");
        return consumed;
      }
      if (msg[nr_consumed + parser._blob_size] != '\r' ||
          msg[nr_consumed + parser._blob_size + 1] != '\n') {
        enqueue_response(conn, sequence, "CLIENT_ERROR bad data chunk\r\n");
        return consumed;
      }
      auto hash = sphinx::logmem::Object::hash_of(key);
      auto target_id = find_target(hash);
      std::string_view blob{parser._blob_start, parser._blob_size};
      auto flags = static_cast<uint32_t>(parser._flags);
      auto expiration = normalize_expiration(parser._expiration);
      if (target_id == _reactor->thread_id()) {
        switch (op) {
          case Opcode::Set:
            cmd_set(target_id, conn->id, sequence, key, blob, flags, expiration);
            break;
          case Opcode::Add:
            cmd_add(target_id, conn->id, sequence, key, blob, flags, expiration);
            break;
          case Opcode::Replace:
            cmd_replace(target_id, conn->id, sequence, key, blob, flags, expiration);
            break;
          default:
            break;
        }
      } else {
        Message* cmd = new Message();
        cmd->connection_id = conn->id;
        cmd->sequence = sequence;
        cmd->source_thread = static_cast<uint8_t>(_reactor->thread_id());
        cmd->op = op;
        cmd->key_size = static_cast<uint32_t>(key.size());
        cmd->flags = flags;
        cmd->expiration = expiration;
        cmd->buffer.append(key);
        cmd->buffer.append(blob);
        if (!_reactor->send_msg(target_id, cmd)) {
          delete cmd;
          enqueue_response(conn, sequence, "SERVER_ERROR request queue is full\r\n");
        }
      }
      return consumed;
    }
    case Opcode::Get: {
      const auto& key = parser.key();
      auto hash = sphinx::logmem::Object::hash_of(key);
      auto target_id = find_target(hash);
      if (target_id == _reactor->thread_id()) {
        cmd_get(target_id, conn->id, sequence, key);
      } else {
        Message* cmd = new Message();
        cmd->connection_id = conn->id;
        cmd->sequence = sequence;
        cmd->source_thread = static_cast<uint8_t>(_reactor->thread_id());
        cmd->op = op;
        cmd->key_size = static_cast<uint32_t>(key.size());
        cmd->buffer.append(key);
        if (!_reactor->send_msg(target_id, cmd)) {
          delete cmd;
          enqueue_response(conn, sequence, "SERVER_ERROR request queue is full\r\n");
        }
      }
      return nr_consumed;
    }
  }
  return nr_consumed;
}

void
Server::cmd_set(size_t response_thread,
                uint64_t connection_id,
                uint64_t sequence,
                std::string_view key,
                std::string_view blob,
                uint32_t flags,
                uint64_t expiration)
{
  if (this->_log.append(key, blob, flags, expiration)) {
    send_response(response_thread, connection_id, sequence, "STORED\r\n");
  } else {
    send_response(
      response_thread, connection_id, sequence, "SERVER_ERROR out of memory storing object\r\n");
  }
}

void
Server::cmd_add(size_t response_thread,
                uint64_t connection_id,
                uint64_t sequence,
                std::string_view key,
                std::string_view blob,
                uint32_t flags,
                uint64_t expiration)
{
  if (this->_log.find_value(key)) {
    send_response(response_thread, connection_id, sequence, "NOT_STORED\r\n");
    return;
  }
  if (this->_log.append(key, blob, flags, expiration)) {
    send_response(response_thread, connection_id, sequence, "STORED\r\n");
  } else {
    send_response(
      response_thread, connection_id, sequence, "SERVER_ERROR out of memory storing object\r\n");
  }
}

void
Server::cmd_replace(size_t response_thread,
                    uint64_t connection_id,
                    uint64_t sequence,
                    std::string_view key,
                    std::string_view blob,
                    uint32_t flags,
                    uint64_t expiration)
{
  if (!this->_log.find_value(key)) {
    send_response(response_thread, connection_id, sequence, "NOT_STORED\r\n");
    return;
  }
  if (this->_log.append(key, blob, flags, expiration)) {
    send_response(response_thread, connection_id, sequence, "STORED\r\n");
  } else {
    send_response(
      response_thread, connection_id, sequence, "SERVER_ERROR out of memory storing object\r\n");
  }
}

void
Server::cmd_get(size_t response_thread,
                uint64_t connection_id,
                uint64_t sequence,
                std::string_view key)
{
  std::string response;
  auto search = this->_log.find_value(key);
  if (search) {
    const auto& value = search.value();
    response += "VALUE ";
    response += key;
    response += " ";
    response += sphinx::to_string(value.flags);
    response += " ";
    response += sphinx::to_string(value.blob.size());
    response += "\r\n";
    response += value.blob;
    response += "\r\n";
  }
  response += "END\r\n";
  send_response(response_thread, connection_id, sequence, response);
}

void
Server::send_response(size_t response_thread,
                      uint64_t connection_id,
                      uint64_t sequence,
                      std::string_view msg)
{
  if (response_thread == _reactor->thread_id()) {
    auto it = _connections.find(connection_id);
    if (it != _connections.end()) {
      enqueue_response(it->second, sequence, msg);
    }
    return;
  }
  auto* response = new Message();
  response->type = MessageType::Response;
  response->connection_id = connection_id;
  response->sequence = sequence;
  response->payload = std::string{msg};
  // A full bounded ring is handled by the Reactor's FIFO deferred mailbox, so
  // this data core never blocks while the connection core drains responses.
  // No Socket or epoll state crosses this boundary.
  if (!_reactor->send_msg_deferred(response_thread, response)) {
    // Allocation failure is the only expected failure for the deferred path;
    // make it observable instead of silently stranding the client request.
    std::cerr << "response queue allocation failed for connection " << connection_id << std::endl;
    delete response;
  }
}

void
Server::enqueue_response(const std::shared_ptr<Connection>& conn,
                         uint64_t sequence,
                         std::string_view msg)
{
  if (conn->closed) {
    return;
  }
  conn->pending_responses.emplace(sequence, std::string{msg});
  for (;;) {
    auto it = conn->pending_responses.find(conn->next_response_sequence);
    if (it == conn->pending_responses.end()) {
      return;
    }
    auto sock = conn->socket.lock();
    if (!sock) {
      conn->closed = true;
      conn->pending_responses.clear();
      _connections.erase(conn->id);
      return;
    }
    auto complete = sock->send(it->second.data(), it->second.size(), std::nullopt);
    conn->pending_responses.erase(it);
    conn->next_response_sequence++;
    if (!complete) {
      _reactor->send(sock);
    }
    if (sock->closed()) {
      close_connection(conn, sock);
      return;
    }
  }
}

void
Server::close_connection(const std::shared_ptr<Connection>& conn,
                         const std::shared_ptr<sphinx::reactor::TcpSocket>& sock)
{
  if (conn->closed) {
    return;
  }
  conn->closed = true;
  conn->pending_responses.clear();
  auto it = _connections.find(conn->id);
  if (it != _connections.end() && it->second == conn) {
    _connections.erase(it);
  }
  _reactor->close(sock);
}

uint64_t
Server::normalize_expiration(uint64_t expiration)
{
  if (expiration == 0) {
    return 0;
  }
  constexpr uint64_t thirty_days = 60 * 60 * 24 * 30;
  using namespace std::chrono;
  auto now = uint64_t(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
  if (expiration <= thirty_days) {
    return now + expiration;
  }
  return expiration;
}

size_t
Server::find_target(const sphinx::logmem::Hash& hash) const
{
  size_t nr_threads = _reactor->nr_threads();
  if (nr_threads == 1) {
    return _reactor->thread_id();
  }
  return hash % nr_threads;
}

static void
print_version()
{
  std::cout << "Sphinx " << SPHINX_VERSION << std::endl;
}

static void
print_usage()
{
  std::cout << "Usage: " << program << " [OPTION]..." << std::endl;
  std::cout << "Start the Sphinx daemon." << std::endl;
  std::cout << std::endl;
  std::cout << "Options:" << std::endl;
  std::cout << "  -p, --port number           TCP port to listen to (default: " << DEFAULT_TCP_PORT
            << ")" << std::endl;
  std::cout << "  -U, --port number           UDP port to listen to (default: " << DEFAULT_UDP_PORT
            << ")" << std::endl;
  std::cout << "  -l, --listen address        interface to listen to (default: "
            << DEFAULT_LISTEN_ADDR << ")" << std::endl;
  std::cout << "  -m, --memory-limit number   Memory limit in MB (default: " << DEFAULT_MEMORY_LIMIT
            << ")" << std::endl;
  std::cout << "  -s, --segment-size number   Segment size in MB (default: " << DEFAULT_SEGMENT_SIZE
            << ")" << std::endl;
  std::cout << "  -b, --listen-backlog number Listen backlog size (default: "
            << DEFAULT_LISTEN_BACKLOG << ")" << std::endl;
  std::cout << "  -t, --threads number        number of threads to use (default: "
            << DEFAULT_NR_THREADS << ")" << std::endl;
  std::cout << "  -I, --io-backend name       I/O backend (default: "
            << sphinx::reactor::Reactor::default_backend() << ")" << std::endl;
  std::cout << "  -i, --isolate-cpus list     list of CPUs to isolate application threads"
            << std::endl;
  std::cout << "  -S, --sched-fifo            use SCHED_FIFO scheduling policy" << std::endl;
  std::cout << "      --help                  print this help text and exit" << std::endl;
  std::cout << "      --version               print Sphinx version and exit" << std::endl;
  std::cout << std::endl;
}

static void
print_opt_error(const std::string& option, const std::string& reason)
{
  std::cerr << program << ": " << reason << " '" << option << "' option" << std::endl;
  std::cerr << "Try '" << program << " --help' for more information" << std::endl;
}

static void
print_unrecognized_opt(const std::string& option)
{
  print_opt_error(option, "unregonized");
}

static std::set<int>
parse_cpu_list(const std::string& raw_cpu_list)
{
  std::set<int> cpu_list;
  std::istringstream iss(raw_cpu_list);
  std::string token;
  while (std::getline(iss, token, ',')) {
    auto cpu = std::stoi(token);
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
      throw std::invalid_argument("CPU id is out of range");
    }
    cpu_list.emplace(cpu);
  }
  return cpu_list;
}

static Args
parse_cmd_line(int argc, char* argv[])
{
  static struct option long_options[] = {{"port", required_argument, 0, 'p'},
                                         {"listen", required_argument, 0, 'l'},
                                         {"memory-limit", required_argument, 0, 'm'},
                                         {"segment-size", required_argument, 0, 's'},
                                         {"listen-backlog", required_argument, 0, 'b'},
                                         {"threads", required_argument, 0, 't'},
                                         {"io-backend", required_argument, 0, 'I'},
                                         {"isolate-cpus", required_argument, 0, 'i'},
                                         {"sched-fifo", no_argument, 0, 'S'},
                                         {"help", no_argument, 0, 'h'},
                                         {"version", no_argument, 0, 'v'},
                                         {0, 0, 0, 0}};
  Args args;
  int opt, long_index;
  while ((opt = ::getopt_long(argc, argv, "p:l:m:s:b:t:I:i:S", long_options, &long_index)) != -1) {
    switch (opt) {
      case 'p':
        args.tcp_port = std::stoi(optarg);
        break;
      case 'l':
        args.listen_addr = optarg;
        break;
      case 'm':
        args.memory_limit = std::stoi(optarg);
        break;
      case 's':
        args.segment_size = std::stoi(optarg);
        break;
      case 'b':
        args.listen_backlog = std::stoi(optarg);
        break;
      case 't':
        args.nr_threads = std::stoi(optarg);
        break;
      case 'I':
        args.backend = optarg;
        break;
      case 'i':
        args.isolate_cpus = parse_cpu_list(optarg);
        break;
      case 'S':
        args.sched_fifo = true;
        break;
      case 'h':
        print_usage();
        std::exit(EXIT_SUCCESS);
      case 'v':
        print_version();
        std::exit(EXIT_SUCCESS);
      case '?':
        print_unrecognized_opt(argv[optind - 1]);
        std::exit(EXIT_FAILURE);
      default:
        print_usage();
        std::exit(EXIT_FAILURE);
    }
  }
  if (args.tcp_port < 0 || args.tcp_port > 65535) {
    throw std::invalid_argument("TCP port must be between 0 and 65535");
  }
  if (args.memory_limit <= 0) {
    throw std::invalid_argument("memory limit must be positive");
  }
  if (args.segment_size <= 0) {
    throw std::invalid_argument("segment size must be positive");
  }
  if (args.listen_backlog <= 0) {
    throw std::invalid_argument("listen backlog must be positive");
  }
  if (args.nr_threads <= 0 || args.nr_threads > sphinx::reactor::max_nr_threads) {
    throw std::invalid_argument("thread count must be between 1 and " +
                                std::to_string(sphinx::reactor::max_nr_threads));
  }
  if (args.memory_limit % args.nr_threads != 0) {
    throw std::invalid_argument("memory limit (" + std::to_string(args.memory_limit) +
                                ") is not divisible by number of threads (" +
                                std::to_string(args.nr_threads) +
                                "), which is required for partitioning");
  }
  auto per_thread_memory = uint64_t(args.memory_limit / args.nr_threads) * 1024 * 1024;
  auto segment_bytes = uint64_t(args.segment_size) * 1024 * 1024;
  if (segment_bytes > per_thread_memory || per_thread_memory % segment_bytes != 0) {
    throw std::invalid_argument("per-thread memory must contain whole segments");
  }
  return args;
}

void
server_thread(size_t thread_id, std::optional<int> cpu_id, const Args& args)
{
  try {
    if (cpu_id) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(*cpu_id, &cpuset);
      auto err = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
      if (err != 0) {
        throw std::system_error(err, std::system_category(), "pthread_setaffinity_np");
      }
    }
    if (args.sched_fifo) {
      ::sched_param param = {};
      param.sched_priority = 1;
      auto err = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
      if (err != 0) {
        throw std::system_error(errno, std::system_category(), "pthread_setschedparam");
      }
    }
    size_t mem_size = size_t(args.memory_limit) * 1024 * 1024;
    sphinx::memory::Memory memory = sphinx::memory::Memory::mmap(mem_size / args.nr_threads);
    sphinx::logmem::LogConfig log_cfg;
    log_cfg.segment_size = size_t(args.segment_size) * 1024 * 1024;
    log_cfg.memory_ptr = reinterpret_cast<char*>(memory.addr());
    log_cfg.memory_size = memory.size();
    Server server{log_cfg, args.backend, thread_id, size_t(args.nr_threads)};
    server.serve(args);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

struct CpuAffinity
{
  std::set<int> isolate_cpus;
  std::optional<int> next_id;
  CpuAffinity(const std::set<int> isolate_cpus)
    : isolate_cpus{isolate_cpus}
  {
  }
  int next_cpu_id()
  {
    int id = next_id.value_or(0);
    for (;;) {
      if (isolate_cpus.count(id) == 0) {
        break;
      }
      id++;
    }
    next_id = id + 1;
    return id;
  }
};

int
main(int argc, char* argv[])
{
  static_assert(sizeof(Message) <= 3 * sphinx::hardware::cache_line_size);
  try {
    program = ::basename(argv[0]);
    auto args = parse_cmd_line(argc, argv);
    CpuAffinity cpu_affinity{args.isolate_cpus};
    std::vector<std::thread> threads;
    for (int i = 0; i < args.nr_threads; i++) {
      auto thread = std::thread{server_thread, i, cpu_affinity.next_cpu_id(), args};
      threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
      t.join();
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    std::exit(EXIT_FAILURE);
  }
  std::exit(EXIT_SUCCESS);
}
