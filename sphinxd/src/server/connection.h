// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/buffer.h>
#include <sphinx/reactor.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace sphinx::server {

// 管理每个客户端的接收状态、响应顺序和多键查询结果组装。
class Connection final {
 public:
  enum class WriteStatus : uint8_t {
    Complete,
    SocketUnavailable,
    SocketClosed,
  };
  explicit Connection(uint64_t id) : _id{id} {
  }

  uint64_t id() const noexcept {
    return _id;
  }
  bool closed() const noexcept {
    return _closed;
  }
  void mark_closed() {
    _closed = true;
    _pending_responses.clear();
    _pending_multi_gets.clear();
  }
  void set_socket(const std::shared_ptr<sphinx::reactor::TcpSocket>& socket) {
    _socket = socket;
  }
  std::shared_ptr<sphinx::reactor::TcpSocket> socket() const {
    return _socket.lock();
  }

  sphinx::buffer::Buffer& receive_buffer() {
    return _receive_buffer;
  }
  const sphinx::buffer::Buffer& receive_buffer() const {
    return _receive_buffer;
  }
  uint64_t next_request_sequence() noexcept {
    return _next_request_sequence++;
  }
  void rollback_request_sequence() noexcept {
    if (_next_request_sequence != 0) {
      --_next_request_sequence;
    }
  }
  void begin_multi_get(uint64_t sequence, size_t key_count);
  // 仅在所有键的回复片段都到达后返回响应。
  std::optional<std::string> add_multi_get_piece(
      uint64_t sequence,  // NOLINT(bugprone-easily-swappable-parameters)：参数含义由调用处明确区分
      uint32_t key_index, std::string_view payload, bool failed = false);
  // 将响应入队并发送连续就绪的条目；部分 I/O 由反应器处理。
  WriteStatus enqueue_response(uint64_t sequence, std::string_view payload,
                               sphinx::reactor::Reactor& reactor);

 private:
  struct MultiGetState {
    size_t pending = 0;
    bool failed = false;
    std::vector<std::string> pieces;
  };
  uint64_t _id;
  sphinx::buffer::Buffer _receive_buffer;
  uint64_t _next_request_sequence = 0;
  uint64_t _next_response_sequence = 0;
  std::map<uint64_t, std::string> _pending_responses;
  std::map<uint64_t, MultiGetState> _pending_multi_gets;
  std::weak_ptr<sphinx::reactor::TcpSocket> _socket;
  bool _closed = false;
};
}  // namespace sphinx::server
