// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/cluster.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace sphinx {

// 一次操作共用连接、写入和读取的截止时间；连接在多次操作间复用。
class TcpTransport final {
 public:
  TcpTransport(const Node& node, std::chrono::milliseconds timeout);
  ~TcpTransport();
  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  std::string_view target() const;
  void begin_operation();
  void write_all(std::string_view message);
  std::string read_line();
  std::string read_exact(size_t size);

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace sphinx
