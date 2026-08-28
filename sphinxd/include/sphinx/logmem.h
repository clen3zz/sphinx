// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sphinx/index.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

/// \defgroup logmem-module Log-structured memory allocator.
///
/// Log-structured memory allocator manages main memory as a log to improve
/// memory utilization. The allocator manages memory in fixed-size segments,
/// which are arranged as vector of lists, sorted by amount of memory remaining
/// for allocation in the segment. Segments can hold objects of different sizes,
/// which eliminates internal fragmentation (object size being smaller than
/// allocation size) and also reduces external fragmentation (available memory
/// is in such small blocks that objects cannot be allocated from them) because
/// segments are expired in full.
///
/// The main entry point to the log-structured memory allocator is the \ref
/// Log::append() function, which attempts to append a key-value pair to the
/// log. The function allocates memory one segment at a time. That is, all
/// allocations are satisfied by the same segment until it runs out of memory.
/// Furthermore, the allocator first exhausts all memory it manages before
/// attempting to reclaim space by expiring segments.

namespace sphinx::logmem {

/// \addtogroup logmem-module
/// @{

/// Object hash type.
using Hash = uint64_t;

/// Object key type.
using Key = std::string_view;

/// Object blob type.
using Blob = std::string_view;

/// An object in a segment of a log.
class Object final {
  uint32_t _key_size;
  uint32_t _blob_size;
  uint32_t _flags;
  uint32_t _expiration;
  uint32_t _expired;

 public:
  /// \brief Return the size of an object of \ref key and \ref blob.
  static size_t size_of(const Key& key, const Blob& blob);
  /// \brief Return the size of an object of \ref key_size and \ref blob_size.
  static size_t size_of(size_t key_size, size_t blob_size);
  /// \brief Return the hash of \ref key.
  static Hash hash_of(const Key& key);
  /// \brief 使用可选的 Memcached 元数据构造对象。
  Object(const Key& key, const Blob& blob, uint32_t flags = 0, uint64_t expiration = 0);
  /// \brief Expire object.
  void expire();
  /// \brief 返回对象是否已达到其墙钟过期时间。
  bool is_expired(uint64_t now) const;
  /// \brief 返回对象中存储的标志位。
  uint32_t flags() const;
  /// \brief 返回绝对 Unix 过期时间；零表示永不过期。
  uint64_t expiration() const;
  /// \brief Returns the size of the object in memory.
  size_t size() const;
  /// \brief 返回对象键。
  Key key() const;
  /// \brief 返回对象数据。
  Blob blob() const;

 private:
  const char* key_start() const;
  const char* blob_start() const;
};

/// A segment in a log.
class Segment {
  char* _pos;
  char* _end;

 public:
  /// \brief Construct a \ref Segment instance.
  explicit Segment(size_t size);
  /// \brief Return true if segment has no objects; otherwise return false;
  bool is_empty() const;
  /// \brief Returns the number of bytes allocated for objects in this segment.
  size_t size() const;
  /// \brief Reset the segment into a clean segment.
  void reset();
  /// \brief 使用可选的 Memcached 元数据追加对象。
  Object* append(const Key& key, const Blob& blob, uint32_t flags = 0, uint64_t expiration = 0);
  /// \brief Return a pointer to the first object in the segment.
  Object* first_object();
  /// \brief Return a pointer to the next object immediatelly following \ref object.
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

/// A log of objects.
class Log {
  sphinx::index::Index<Key, Object*> _index;
  std::vector<Segment*> _segment_ring;
  size_t _segment_ring_head = 0;
  size_t _segment_ring_tail = 0;
  LogConfig _config;

 public:
  /// \brief Construct a \ref Log instance.
  explicit Log(const LogConfig& config);
  /// \brief Find for a blob for a given \ref key from the log.
  std::optional<Blob> find(const Key& key) const;
  /// \brief 查找值及其 Memcached 元数据。
  std::optional<Value> find_value(const Key& key);
  /// \brief 追加带可选 Memcached 元数据的键值对。
  bool append(const Key& key, const Blob& blob, uint32_t flags = 0, uint64_t expiration = 0);
  /// \brief Remove the given \ref key from the log.
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
}  // namespace sphinx::logmem
