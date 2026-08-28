// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/stats.h>

#include <iterator>
#include <utility>

namespace sphinx::stats {

namespace {

using Counter = ServerStats::Counter;

// 计数器元数据项：关联计数器枚举类型及其对外展示的协议名称
struct CounterInfo {
  Counter counter;
  const char* name;
};

// 计数器元数据查找表：与 ServerStats::Counter 枚举严格对应
constexpr std::array<CounterInfo, static_cast<size_t>(Counter::Count)> kCounterNames = {{
    {Counter::CmdGet, "cmd_get"},
    {Counter::GetHits, "get_hits"},
    {Counter::GetMisses, "get_misses"},
    {Counter::CmdSet, "cmd_set"},
    {Counter::CmdAdd, "cmd_add"},
    {Counter::CmdReplace, "cmd_replace"},
    {Counter::CmdDelete, "cmd_delete"},
    {Counter::CmdIncr, "cmd_incr"},
    {Counter::CmdDecr, "cmd_decr"},
}};

// 编译期断言：确保查找表项数与计数器总数一致，防止遗漏
static_assert(std::size(kCounterNames) == static_cast<size_t>(Counter::Count));

}  // namespace

// 基于 Config 结构体构造服务端统计对象
ServerStats::ServerStats(Config config)
    : _version{std::move(config.version)},
      _threads{config.threads},
      _limit_maxbytes{config.limit_maxbytes} {
}

// 便利构造函数：传入版本字符串、工作线程数以及最大内存限制
ServerStats::ServerStats(std::string version, uint64_t threads, uint64_t limit_maxbytes)
    : ServerStats(Config{std::move(version), threads, limit_maxbytes}) {
}

// 原子累加指定类型的计数器
void ServerStats::increment(Counter counter, uint64_t amount) noexcept {
  const auto index = static_cast<size_t>(counter);

  // 边界检查：确保索引在计数器数组范围内
  if (index < _counters.size()) {
    // 使用松散内存序（relaxed）保证统计计数的高吞吐与低开销
    _counters[index].fetch_add(amount, std::memory_order_relaxed);
  }
}

// 原子读取指定类型的计数器当前值
uint64_t ServerStats::counter(Counter counter) const noexcept {
  const auto index = static_cast<size_t>(counter);

  // 边界检查：越界则安全回退返回 0
  if (index < _counters.size()) {
    return _counters[index].load(std::memory_order_relaxed);
  }

  return 0;
}

// 格式化输出 Memcached 协议兼容的 STAT 统计信息字符串
std::string ServerStats::render() const {
  std::string response;
  // 预分配足够的空间，减少字符串追加时的重新分配
  response.reserve(256);

  // 1. 拼接静态基础信息（版本、线程数、最大内存）
  response += "STAT version ";
  response += _version;
  response += "\r\n";

  response += "STAT threads ";
  response += std::to_string(_threads);
  response += "\r\n";

  response += "STAT limit_maxbytes ";
  response += std::to_string(_limit_maxbytes);
  response += "\r\n";

  // 2. 遍历并拼接所有运行态原子计数器指标
  for (const auto& [counter_type, name] : kCounterNames) {
    response += "STAT ";
    response += name;
    response += ' ';
    response += std::to_string(counter(counter_type));
    response += "\r\n";
  }

  // 3. 按照 Memcached 协议规范以 END\r\n 结尾
  response += "END\r\n";

  return response;
}

}  // namespace sphinx::stats
