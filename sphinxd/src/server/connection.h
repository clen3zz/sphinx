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
namespace sphinx {

// 客户端连接上下文管理类：
// 管理每个 TCP 连接的接收缓冲区、请求与响应序列号、multi-get 聚合拼装以及顺序响应回写
class Connection final {
 public:
  // 数据回写状态枚举
  enum class WriteStatus : uint8_t {
    Complete,           // 写操作已全部完成或已交由 Reactor 异步处理
    SocketUnavailable,  // 底层 Socket 无法获取（已被释放）
    SocketClosed,       // 写入检测到 Socket 已关闭
  };

  explicit Connection(uint64_t id) : _id{id} {}

  // 获取连接唯一 ID
  uint64_t id() const noexcept { return _id; }

  // 连接是否已标记关闭
  bool closed() const noexcept { return _closed; }

  // 标记连接关闭并清理所有挂起的响应与 multi-get 聚合状态
  void mark_closed() {
    _closed = true;
    _pending_responses.clear();
    _pending_multi_gets.clear();
  }

  // 设置关联的底层 TCP Socket 弱引用
  void set_socket(const std::shared_ptr<TcpSocket>& socket) { _socket = socket; }

  // 获取关联的底层 TCP Socket（提升为 shared_ptr）
  std::shared_ptr<TcpSocket> socket() const { return _socket.lock(); }

  // 获取接收缓冲区可变引用
  Buffer& receive_buffer() { return _receive_buffer; }

  // 获取接收缓冲区常量引用
  const Buffer& receive_buffer() const { return _receive_buffer; }

  // 生成递增的下一个请求序号
  uint64_t next_request_sequence() noexcept { return _next_request_sequence++; }

  // 回滚请求序号（用于解析错误或失败回滚）
  void rollback_request_sequence() noexcept {
    if (_next_request_sequence != 0) {
      --_next_request_sequence;
    }
  }

  // 开启 multi-get 请求跟踪，预分配指定数量的子响应分片
  void begin_multi_get(uint64_t sequence, size_t key_count);

  // 记录 multi-get 请求的一个分片响应，当且仅当所有分片都就绪时返回拼装后的完整响应体
  std::optional<std::string> add_multi_get_piece(uint64_t sequence, uint32_t key_index,
                                                 std::string_view payload, bool failed = false);

  // 将响应入队并按序列号顺序尝试写出就绪数据（部分 I/O 由 Reactor 异步写入）
  WriteStatus enqueue_response(uint64_t sequence, std::string_view payload, Reactor& reactor);

 private:
  // multi-get 聚合状态结构体
  struct MultiGetState {
    size_t pending = 0;               // 剩余未到达的分片计数
    bool failed = false;              // 是否有任一分片执行失败
    std::vector<std::string> pieces;  // 已接收的分片数据缓存
  };

  uint64_t _id;                                           // 连接全局唯一 ID
  Buffer _receive_buffer;                                 // TCP 读入数据缓冲区
  uint64_t _next_request_sequence = 0;                    // 下一个分发的请求序号
  uint64_t _next_response_sequence = 0;                   // 期待写出的下一个响应序号
  std::map<uint64_t, std::string> _pending_responses;     // 乱序到达暂存的响应队列
  std::map<uint64_t, MultiGetState> _pending_multi_gets;  // 正在聚合的 multi-get 状态字典
  std::weak_ptr<TcpSocket> _socket;                       // 底层套接字弱引用
  bool _closed = false;                                   // 连接是否已关闭
};

}  // namespace sphinx
