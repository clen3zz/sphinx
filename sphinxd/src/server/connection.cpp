// SPDX-License-Identifier: Apache-2.0
#include "connection.h"
namespace sphinx::server {

void
Connection::begin_multi_get(uint64_t sequence, size_t key_count)
{
  _pending_multi_gets.emplace(sequence,
                              MultiGetState{key_count, false, std::vector<std::string>(key_count)});
}
std::optional<std::string>
Connection::add_multi_get_piece(uint64_t sequence,
                                uint32_t key_index,
                                std::string_view payload,
                                bool failed)
{
  auto it = _pending_multi_gets.find(sequence);
  if (it == _pending_multi_gets.end()) {
    return std::nullopt;
  }
  auto& state = it->second;
  if (state.pending == 0) {
    return std::nullopt;
  }
  if (failed || key_index >= state.pieces.size()) {
    state.failed = true;
  } else {
    state.pieces[key_index] = std::string{payload};
  }
  --state.pending;
  if (state.pending != 0) {
    return std::nullopt;
  }

  std::string response;
  if (state.failed) {
    response = "SERVER_ERROR request queue is full\r\n";
  } else {
    for (const auto& piece : state.pieces) {
      response += piece;
    }
    response += "END\r\n";
  }
  _pending_multi_gets.erase(it);
  return response;
}
Connection::WriteStatus
Connection::enqueue_response(uint64_t sequence,
                             std::string_view payload,
                             sphinx::reactor::Reactor& reactor)
{
  if (_closed) {
    return WriteStatus::SocketUnavailable;
  }
  _pending_responses.emplace(sequence, std::string{payload});
  for (;;) {
    auto it = _pending_responses.find(_next_response_sequence);
    if (it == _pending_responses.end()) {
      return WriteStatus::Complete;
    }
    auto socket = _socket.lock();
    if (!socket) {
      mark_closed();
      return WriteStatus::SocketUnavailable;
    }
    auto complete = socket->send(it->second.data(), it->second.size());
    _pending_responses.erase(it);
    ++_next_response_sequence;
    if (!complete) {
      reactor.send(socket);
    }
    if (socket->closed()) {
      return WriteStatus::SocketClosed;
    }
  }
}
} // namespace sphinx::server
