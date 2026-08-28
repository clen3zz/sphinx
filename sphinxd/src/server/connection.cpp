// SPDX-License-Identifier: Apache-2.0
#include "connection.h"
namespace sphinx {

// 初始化指定序列号的 multi-get 聚合上下文
void Connection::begin_multi_get(uint64_t sequence, size_t key_count) {
  _pending_multi_gets.emplace(sequence,
                              MultiGetState{key_count, false, std::vector<std::string>(key_count)});
}

// 记录 multi-get 请求的一个分片响应；仅当全部子响应集齐后拼接并返回最终响应
std::optional<std::string> Connection::add_multi_get_piece(
    uint64_t sequence,  // NOLINT(bugprone-easily-swappable-parameters)：参数含义由调用处明确区分
    uint32_t key_index, std::string_view payload, bool failed) {
  // 1. 查找对应的 multi-get 上下文
  auto it = _pending_multi_gets.find(sequence);
  if (it == _pending_multi_gets.end()) {
    return std::nullopt;
  }

  [[maybe_unused]] auto& [_, state] = *it;
  if (state.pending == 0) {
    return std::nullopt;
  }

  // 2. 记录分片或标记失败
  if (failed || key_index >= state.pieces.size()) {
    state.failed = true;
  } else {
    state.pieces[key_index] = std::string{payload};
  }

  // 3. 递减等待计数；若尚未完全就绪则提前返回
  --state.pending;
  if (state.pending != 0) {
    return std::nullopt;
  }

  // 4. 全部子响应集齐，构建完整协议响应
  std::string response;
  if (state.failed) {
    response = "SERVER_ERROR request queue is full\r\n";
  } else {
    for (const auto& piece : state.pieces) {
      response += piece;
    }
    response += "END\r\n";
  }

  // 5. 清理已完成的 multi-get 状态
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
  _pending_responses.emplace(sequence, std::string{payload});

  // 3. 循环按 sequence 严格单调递增顺序写出就绪响应
  for (;;) {
    auto it = _pending_responses.find(_next_response_sequence);
    if (it == _pending_responses.end()) {
      // 下一期望序号尚未到达，保持等待
      return WriteStatus::Complete;
    }

    // 获取底层套接字句柄
    auto socket = _socket.lock();
    if (!socket) {
      mark_closed();
      return WriteStatus::SocketUnavailable;
    }

    // 尝试同步写入数据
    auto complete = socket->send(it->second.data(), it->second.size());
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
