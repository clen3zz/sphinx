// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/protocol.h>
#include <sphinx/reactor.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
namespace sphinx::server {

// 跨线程命令消息：自行持有完整数据，在跨 Reactor 队列传递时无需依赖源连接或解析器生命周期
struct Command {
  uint64_t connection_id = 0;  // 来源客户端连接唯一标识
  uint64_t sequence = 0;       // 请求序列号（保障响应乱序返回时按序组装）
  size_t source_thread = 0;    // 发起请求的 Reactor 线程编号
  memcache::Opcode op = memcache::Opcode::Version;  // 操作码
  std::string key;                                                  // 目标键
  std::string blob;                                                 // 键对应的载荷数据
  uint32_t flags = 0;                                               // Memcached 协议 flags
  uint64_t expiration = 0;                                          // 过期时间戳（秒）
  uint64_t delta = 0;                                               // 自增/自减步长
  bool multi_get = false;                                           // 是否为 multi-get 聚合查询命令
  uint32_t key_index = 0;  // multi-get 命令中的子键索引位置

  Command(uint64_t id,  // NOLINT(bugprone-easily-swappable-parameters)：构造点使用明确的字段顺序
          uint64_t request, size_t thread, memcache::Opcode opcode, std::string command_key)
      : connection_id{id},
        sequence{request},
        source_thread{thread},
        op{opcode},
        key{std::move(command_key)} {
  }
};

// 跨线程响应消息：统一表示单个命令的完整响应或 multi-get 命令的子分片响应
struct Response {
  uint64_t connection_id = 0;  // 目标客户端连接唯一标识
  uint64_t sequence = 0;       // 对应请求的序列号
  std::string payload;         // 待回写客户端的序列化数据
  bool multi_get = false;      // 是否为 multi-get 分片响应
  uint32_t key_index = 0;      // multi-get 命令中的子分片索引位置
};

// Reactor 消息支持的载荷类型集合（命令或响应）
using MessagePayload = std::variant<Command, Response>;

// Reactor 基础消息封装，用于在各工作线程的 Reactor 邮箱间投递
struct Message : reactor::Message {
  MessagePayload payload;

  template <typename Payload>
  explicit Message(Payload value) : payload{std::move(value)} {
  }
};

}  // namespace sphinx::server
