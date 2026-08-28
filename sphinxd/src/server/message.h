// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/protocol.h>
#include <sphinx/reactor.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sphinx {

// 跨线程命令消息：自行持有完整数据，在跨 Reactor 队列传递时无需依赖源连接或解析器生命周期
struct Command : Message {
  uint64_t connection_id = 0;   // 来源客户端连接唯一标识
  uint64_t sequence = 0;        // 请求序列号（保障响应乱序返回时按序组装）
  size_t source_thread = 0;     // 发起请求的 Reactor 线程编号
  Opcode op = Opcode::Version;  // 操作码
  std::string key;              // 目标键
  std::string blob;             // 键对应的载荷数据
  uint32_t flags = 0;           // Memcached 协议 flags
  uint64_t expiration = 0;      // 过期时间戳（秒）
  uint64_t delta = 0;           // 自增/自减步长
  bool multi_get = false;       // 是否为 multi-get 聚合查询命令
  uint32_t key_index = 0;       // multi-get 命令中的子键索引位置

  Command(uint64_t id, uint64_t request, size_t thread, Opcode opcode, std::string command_key)
      : connection_id{id},
        sequence{request},
        source_thread{thread},
        op{opcode},
        key{std::move(command_key)} {
  }
};

// 跨线程响应消息：统一表示单个命令的完整响应或 multi-get 命令的子分片响应
struct Response : Message {
  uint64_t connection_id = 0;  // 目标客户端连接唯一标识
  uint64_t sequence = 0;       // 对应请求的序列号
  std::string payload;         // 待回写客户端的序列化数据
  bool multi_get = false;      // 是否为 multi-get 分片响应
  uint32_t key_index = 0;      // multi-get 命令中的子分片索引位置

  Response(uint64_t id, uint64_t request, std::string response_payload, bool is_multi_get,
           uint32_t response_key_index)
      : connection_id{id},
        sequence{request},
        payload{std::move(response_payload)},
        multi_get{is_multi_get},
        key_index{response_key_index} {
  }
};

}  // namespace sphinx
