// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/hardware.h>

#include <array>
#include <atomic>
#include <utility>

/// \defgroup spsc-queue-module
/// 有界单生产者/单消费者（SPSC）无等待（wait-free）且无锁（lock-free）队列
///
/// SPSC 队列是一个环形缓冲区（ring buffer），包含两个环形索引：head 和 tail。
/// 生产者在当前 tail 之后写入新条目，消费者从 head 读取条目。
///
/// 虽然该算法是独立实现的，但参考了以下实现：
///
///  https://www.scylladb.com/2018/02/15/memory-barriers-seastar-linux/
///  https://github.com/rigtorp/SPSCQueue
///  https://github.com/fsaintjacques/disruptor--
///
/// 此外，以下论文中也阐述了相同的算法（包括正确性证明）：
///
///   Lê, N.M., Guatto, A., Cohen, A. and Pop, A., 2013, October. Correct and
///   efficient bounded FIFO queues. In Computer Architecture and High Performance
///   Computing (SBAC-PAD), 2013 25th International Symposium on (pp. 144-151).
///   IEEE.

namespace sphinx {

/// \addtogroup spsc-queue-module
/// @{

template <typename T, size_t N>
class Queue {
  static_assert(N > 1, "SPSC queue capacity must be greater than one");
  alignas(cache_line_size) std::atomic<size_t> _head =
      0;  // 消费者读取头索引（按缓存行对齐避免伪共享）
  alignas(cache_line_size) std::atomic<size_t> _tail =
      0;                   // 生产者写入尾索引（按缓存行对齐避免伪共享）
  std::array<T, N> _data;  // 环形队列存储底层固定大小数组

 public:
  // 检查队列是否为空
  bool empty() const noexcept {
    return _head.load(std::memory_order_acquire) == _tail.load(std::memory_order_acquire);
  }

  // 尝试在队尾就地构造新元素（若队列已满则返回 false）
  template <typename... Args>
  bool try_to_emplace(Args&&... args) noexcept {
    auto tail = _tail.load(std::memory_order_relaxed);
    auto next_tail = tail + 1;
    if (next_tail == N) {
      next_tail = 0;
    }
    if (next_tail == _head.load(std::memory_order_acquire)) {
      return false;
    }
    _data[tail] = T{std::forward<Args>(args)...};
    // release 存储确保消息构造完成后才更新 tail，避免消费者读取过期消息。
    _tail.store(next_tail, std::memory_order_release);
    return true;
  }

  // 获取队头元素指针（若队列为空则返回 nullptr）
  T* front() noexcept {
    auto head = _head.load(std::memory_order_relaxed);
    if (_tail.load(std::memory_order_acquire) == head) {
      return nullptr;
    }
    return &_data[head];
  }

  // 弹出队头元素并推进 head 索引
  void pop() noexcept {
    auto head = _head.load(std::memory_order_relaxed);
    auto next_head = head + 1;
    if (next_head == N) {
      next_head = 0;
    }
    // release 存储确保消费者读完槽位后，生产者才可以重新使用它。
    _head.store(next_head, std::memory_order_release);
  }
};

/// @}
}  // namespace sphinx
