// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/index.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

/// \defgroup logmem-module 日志结构化内存分配器
///
/// 日志结构化内存分配器将主内存作为一段日志来管理，以提高
/// 内存利用率。该分配器以固定大小的 segment（段）为单位管理内存。
/// 这些 segment 被组织成一个由多个链表组成的 vector，并按照每个
/// segment 中剩余可分配内存的大小进行排序。
///
/// 一个 segment 中可以存放不同大小的对象，因此可以消除内部碎片
/// （即对象大小小于实际分配给它的空间）。同时，它也能减少外部碎片
/// （即可用内存被分散成很多过小的块，导致无法为对象分配连续空间），
/// 因为 segment 在回收时是整个一起失效并释放的。
///
/// 日志结构化内存分配器的主要入口是 \ref Log::append() 函数，
/// 它会尝试向日志中追加一个键值对。
///
/// 该函数每次以一个 segment 为单位进行内存分配。也就是说，
/// 在当前 segment 的空间耗尽之前，所有的内存分配请求都会由同一个
/// segment 来满足。
///
/// 此外，分配器会优先使用完它当前管理的所有可用内存，只有在这些
/// 内存全部耗尽之后，才会尝试通过让某些 segment 失效（expire）
/// 来回收空间。

namespace sphinx {

/// \addtogroup logmem-module
/// @{

/// 对象哈希类型。
using Hash = uint64_t;

/// 对象键（Key）类型。
using Key = std::string_view;

/// 对象数据（Blob）类型。
using Blob = std::string_view;

/// 日志段（segment）中的对象。
class Object final {
  uint32_t _key_size;
  uint32_t _blob_size;
  uint32_t _flags;
  uint32_t _expiration;
  uint32_t _expired;

 public:
  /// \brief 返回由 \ref key 和 \ref blob 组成的对象大小。
  static size_t size_of(const Key& key, const Blob& blob);
  /// \brief 返回指定 \ref key_size 和 \ref blob_size 的对象大小。
  static size_t size_of(size_t key_size, size_t blob_size);
  /// \brief 返回 \ref key 的哈希值。
  static Hash hash_of(const Key& key);
  /// \brief 使用可选的 Memcached 元数据构造对象。
  Object(const Key& key, const Blob& blob, uint32_t flags = 0, uint64_t expiration = 0);
  /// \brief 主动标记对象为已过期失效。
  void expire();
  /// \brief 返回对象是否已达到其墙钟过期时间。
  bool is_expired(uint64_t now) const;
  /// \brief 返回对象中存储的标志位。
  uint32_t flags() const;
  /// \brief 返回绝对 Unix 过期时间；零表示永不过期。
  uint64_t expiration() const;
  /// \brief 返回对象在内存中的大小。
  size_t size() const;
  /// \brief 返回对象键。
  Key key() const;
  /// \brief 返回对象数据。
  Blob blob() const;

 private:
  const char* key_start() const;
  const char* blob_start() const;
};

/// 日志中的段（segment）。
class Segment {
  char* _pos;
  char* _end;

 public:
  /// \brief 构造一个 \ref Segment 实例。
  explicit Segment(size_t size);
  /// \brief 如果段中尚未包含任何对象则返回 true，否则返回 false。
  bool is_empty() const;
  /// \brief 返回该段中已为对象分配的字节数。
  size_t size() const;
  /// \brief 重置段为空净状态。
  void reset();
  /// \brief 使用可选的 Memcached 元数据追加对象。
  Object* append(const Key& key, const Blob& blob, uint32_t flags = 0, uint64_t expiration = 0);
  /// \brief 返回指向段中第一个对象的指针。
  Object* first_object();
  /// \brief 返回紧随 \ref object 之后的下一个对象的指针。
  Object* next_object(Object* object) const;

 private:
  char* start();
  const char* start() const;
};

struct LogConfig {
  char* memory_ptr;
  size_t memory_size;
  size_t segment_size;
};

/// 一个值及其 Memcached 元数据。
struct Value {
  Blob blob;
  uint32_t flags;
  uint64_t expiration;
};

/// 原子计数器更新的结果状态。
enum class ArithmeticStatus {
  Success,
  NotFound,
  NonNumeric,
  StorageFull,
};

/// 原子计数器更新的结果。
struct ArithmeticResult {
  ArithmeticStatus status;
  uint64_t value;
};

/// 对象的日志存储引擎。
class Log {
  Index<Key, Object*> _index;
  std::vector<Segment*> _segment_ring;
  size_t _segment_ring_head = 0;
  size_t _segment_ring_tail = 0;
  LogConfig _config;

 public:
  /// \brief 构造一个 \ref Log 实例。
  explicit Log(const LogConfig& config);
  /// \brief 从日志中查找给定 \ref key 对应的数据（blob）。
  std::optional<Blob> find(const Key& key) const;
  /// \brief 查找值及其 Memcached 元数据。
  std::optional<Value> find_value(const Key& key);
  /// \brief 追加带可选 Memcached 元数据的键值对。
  bool append(const Key& key, const Blob& blob, uint32_t flags = 0, uint64_t expiration = 0);
  /// \brief 从日志中移除给定的 \ref key。
  bool remove(const Key& key);
  /// \brief 在保留元数据的同时递增十进制计数器。
  ArithmeticResult incr(const Key& key, uint64_t delta);
  /// \brief 在保留元数据的同时递减十进制计数器。
  ArithmeticResult decr(const Key& key, uint64_t delta);

 private:
  bool try_to_append(const Key& key, const Blob& blob, uint32_t flags, uint64_t expiration);
  bool try_to_append(Segment* segment, const Key& key, const Blob& blob, uint32_t flags,
                     uint64_t expiration);
  size_t expire(size_t reclaim_target);
  size_t expire(Segment* segment);
  ArithmeticResult update_counter(const Key& key, uint64_t delta, bool increment);
};

/// @}
}  // namespace sphinx
