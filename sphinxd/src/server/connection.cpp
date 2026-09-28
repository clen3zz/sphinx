// SPDX-License-Identifier: Apache-2.0
#include "connection.h"
namespace sphinx {

// 初始化指定序列号的 multi-get 聚合上下文
bool Connection::begin_multi_get(uint64_t sequence, size_t key_count) {
  constexpr size_t max_pending_multi_get_slots = 4096;
  if (_closed || key_count == 0 ||
      key_count > max_pending_multi_get_slots - _pending_multi_get_slots) {
    return false;
  }
  const auto [it, inserted] = _pending_multi_gets.emplace(
      sequence,
      MultiGetState{key_count, false, false, 0, key_count, std::vector<std::string>(key_count)});
  (void)it;
  if (inserted) {
    _pending_multi_get_slots += key_count;
  }
  return inserted;
}

// 记录 multi-get 请求的一个分片响应；仅当全部子响应集齐后拼接并返回最终响应
std::optional<std::string> Connection::add_multi_get_piece(uint64_t sequence, uint32_t key_index,
                                                           std::string_view payload, bool failed) {
  // 1. 查找对应的 multi-get 上下文
  const auto it = _pending_multi_gets.find(sequence);
  if (it == _pending_multi_gets.end()) {
    return std::nullopt;
  }

  auto& state = it->second;

  // 2. 记录分片或标记失败
  if (failed || (!state.resource_limit && key_index >= state.pieces.size())) {
    state.failed = true;
  } else if (!state.failed && !state.resource_limit) {
    if (payload.size() > max_connection_response_bytes - _pending_multi_get_bytes) {
      state.resource_limit = true;
      _pending_multi_get_bytes -= state.buffered_bytes;
      state.buffered_bytes = 0;
      state.pieces.clear();
    } else if (state.pieces[key_index].empty()) {
      state.pieces[key_index] = std::string{payload};
      state.buffered_bytes += payload.size();
      _pending_multi_get_bytes += payload.size();
    }
  }

  // 3. 递减等待计数；若尚未完全就绪则提前返回
  if (state.pending == 0) {
    return std::nullopt;
  }
  --state.pending;
  if (state.pending != 0) {
    return std::nullopt;
  }

  // 4. 全部子响应集齐，构建完整协议响应
  std::string response;
  if (state.failed) {
    response = "SERVER_ERROR request queue is full\r\n";
  } else if (state.resource_limit) {
    response = "SERVER_ERROR response too large\r\n";
  } else {
    for (const auto& piece : state.pieces) {
      response += piece;
    }
    response += "END\r\n";
  }

  // 5. 清理已完成的 multi-get 状态
  _pending_multi_get_bytes -= state.buffered_bytes;
  _pending_multi_get_slots -= state.slots;
  _pending_multi_gets.erase(it);

  return response;
}

// 将响应按序列号入队，并按严格连续顺序写出就绪的响应
Connection::WriteStatus Connection::enqueue_response(uint64_t sequence, std::string_view payload,
                                                     Reactor& reactor) {
  // 1. 连接已关闭则拒绝入队
  if (_closed) {
    return WriteStatus::SocketUnavailable;
  }

  // 2. 将响应放入待发映射表中（键为请求序列号）
  if (payload.size() > max_connection_response_bytes - _pending_response_bytes) {
    return WriteStatus::ResourceLimit;
  }
  const auto [inserted, new_entry] = _pending_responses.emplace(sequence, std::string{payload});
  if (!new_entry) {
    return WriteStatus::SocketUnavailable;
  }
  _pending_response_bytes += inserted->second.size();

  // 3. 循环按 sequence 严格单调递增顺序写出就绪响应
  while (true) {
    const auto it = _pending_responses.find(_next_response_sequence);
    if (it == _pending_responses.end()) {
      // 下一期望序号尚未到达，保持等待
      return WriteStatus::Complete;
    }

    // 获取底层套接字句柄
    const auto socket = _socket.lock();
    if (!socket) {
      mark_closed();
      return WriteStatus::SocketUnavailable;
    }

    // 尝试同步写入数据
    const auto complete = socket->send(it->second.data(), it->second.size());
    _pending_response_bytes -= it->second.size();
    _pending_responses.erase(it);
    ++_next_response_sequence;

    // 若数据未能完全写入内核缓冲区，注册到 Reactor 异步继续发送
    if (!complete) {
      reactor.send(socket);
    }

    // 检测写入后套接字是否发生关闭
    if (socket->closed()) {
      return WriteStatus::SocketClosed;
    }
  }
}

}  // namespace sphinx
