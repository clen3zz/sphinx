// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <MurmurHash3.h>
#include <sphinx/logmem.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace sphinx::logmem {

// 获取当前系统时间的 UNIX 秒级时间戳
static uint64_t current_time_seconds() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// 解析以十进制字符存储的 64 位无符号整数（带严格溢出检测）
static std::optional<uint64_t> parse_uint64_decimal(const Blob& blob) {
  if (blob.empty()) {
    return std::nullopt;
  }

  uint64_t value = 0;
  for (char digit : blob) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }

    auto numeric_digit = static_cast<uint64_t>(digit - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - numeric_digit) / 10) {
      return std::nullopt;
    }

    value = value * 10 + numeric_digit;
  }

  return value;
}

// Object 构造函数：在 placement new 分配的连续内存块中初始化头信息并将 key 和 blob 拷贝至尾部
Object::Object(const Key& key, const Blob& blob, uint32_t flags, uint64_t expiration)
    : _key_size{static_cast<uint32_t>(key.size())},
      _blob_size{static_cast<uint32_t>(blob.size())},
      _flags{flags},
      _expiration{static_cast<uint32_t>(expiration)},
      _expired{0} {
  // 1. 校验过期时间戳上限
  if (expiration > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("expiration is too large");
  }

  // 2. 拷贝变长 Key 数据
  if (_key_size != 0) {
    std::memcpy(const_cast<char*>(key_start()), key.data(), _key_size);
  }

  // 3. 拷贝变长 Blob 数据
  if (_blob_size != 0) {
    std::memcpy(const_cast<char*>(blob_start()), blob.data(), _blob_size);
  }
}

// 计算给定 Key 与 Blob 构造 Object 所需的总对齐字节数
size_t Object::size_of(const Key& key, const Blob& blob) {
  return Object::size_of(key.size(), blob.size());
}

// 计算指定长度下的 Object 内存对齐尺寸
size_t Object::size_of(size_t key_size, size_t blob_size) {
  if (key_size > std::numeric_limits<uint32_t>::max() ||
      blob_size > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("object is too large");
  }

  auto raw_size = sizeof(Object) + key_size + blob_size;
  constexpr auto alignment = alignof(Object);
  if (raw_size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
    throw std::invalid_argument("object is too large");
  }

  return (raw_size + alignment - 1) / alignment * alignment;
}

// 计算键的 MurmurHash3 哈希值
Hash Object::hash_of(const Key& key) {
  if (key.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("key is too large to hash");
  }

  if (key.empty()) {
    return 0;
  }

  uint32_t hash = 0;
  MurmurHash3_x86_32(key.data(), static_cast<int>(key.size()), 1, &hash);
  return hash;
}

// 获取当前对象的实际对齐大小
size_t Object::size() const {
  return Object::size_of(_key_size, _blob_size);
}

// 主动标记对象为已过期失效
void Object::expire() {
  _expired = 1;
}

// 检查对象是否已失效（已标记失效或已超过指定的过期时间戳）
bool Object::is_expired(uint64_t now) const {
  return _expired != 0 || (_expiration != 0 && _expiration <= now);
}

uint32_t Object::flags() const {
  return _flags;
}

uint64_t Object::expiration() const {
  return _expiration;
}

Key Object::key() const {
  return Key{key_start(), _key_size};
}

Blob Object::blob() const {
  return Blob{blob_start(), _blob_size};
}

// 获取 Key 在对象尾部连续内存中的起始指针
const char* Object::key_start() const {
  const char* obj_start = reinterpret_cast<const char*>(this);
  return obj_start + sizeof(Object);
}

// 获取 Blob 在对象尾部连续内存中的起始指针
const char* Object::blob_start() const {
  return key_start() + _key_size;
}

// Segment 构造函数：初始化段内空闲写入指针与段末尾界限
Segment::Segment(size_t size) : _pos{nullptr}, _end{nullptr} {
  if (size < sizeof(Segment)) {
    throw std::invalid_argument("segment size is too small");
  }

  _pos = start();
  _end = start() + (size - sizeof(Segment));
}

// 段当前是否尚未写入任何对象
bool Segment::is_empty() const {
  return _pos == start();
}

// 获取段可用数据区域总容量（扣除 Segment 头）
size_t Segment::size() const {
  return static_cast<size_t>(_end - start());
}

// 重置段写入指针至起始位置（清空段）
void Segment::reset() {
  _pos = start();
}

// 向段内追加单个对象（空间不足则返回 nullptr）
Object* Segment::append(const Key& key, const Blob& blob, uint32_t flags, uint64_t expiration) {
  size_t object_size = Object::size_of(key, blob);
  size_t remaining = static_cast<size_t>(_end - _pos);

  if (remaining >= object_size) {
    Object* object = new (_pos) Object(key, blob, flags, expiration);
    _pos += object_size;
    return object;
  }

  return nullptr;
}

// 获取段内存储的第一个对象指针
Object* Segment::first_object() {
  if (is_empty()) {
    return nullptr;
  }

  return reinterpret_cast<Object*>(start());
}

// 迭代获取段内的下一个对象指针（若已达当前写入位置则返回 nullptr）
Object* Segment::next_object(Object* object) const {
  if (object == nullptr) {
    return nullptr;
  }

  char* next = reinterpret_cast<char*>(object) + object->size();
  if (next >= _pos) {
    return nullptr;
  }

  return reinterpret_cast<Object*>(next);
}

// 段数据区起始指针
char* Segment::start() {
  return reinterpret_cast<char*>(this) + sizeof(Segment);
}

const char* Segment::start() const {
  return reinterpret_cast<const char*>(this) + sizeof(Segment);
}

// Log 构造函数：在预先 mmap 的大块连续内存上划分并初始化环形段列表
Log::Log(const LogConfig& config) : _config{config} {
  auto seg_size = _config.segment_size;
  auto mem_ptr = _config.memory_ptr;
  auto mem_size = _config.memory_size;

  // 1. 内存参数合法性与对齐约束校验
  if (mem_ptr == nullptr || mem_size == 0) {
    throw std::invalid_argument("log memory must not be empty");
  }
  if (seg_size < sizeof(Segment) + Object::size_of(0, 0)) {
    throw std::invalid_argument("segment size is too small");
  }
  if (seg_size > mem_size || mem_size % seg_size != 0) {
    throw std::invalid_argument("log memory must contain whole segments");
  }
  if (seg_size % alignof(Object) != 0) {
    throw std::invalid_argument("segment size is not suitably aligned");
  }
  if (reinterpret_cast<uintptr_t>(mem_ptr) % alignof(Object) != 0) {
    throw std::invalid_argument("log memory is not suitably aligned");
  }

  // 2. 依次在内存块上 placement new 构造各个 Segment 对象并加入环形队列
  _segment_ring.reserve(mem_size / seg_size);
  for (size_t seg_off = 0; seg_off < mem_size; seg_off += seg_size) {
    char* seg_ptr = mem_ptr + seg_off;
    Segment* seg = new (seg_ptr) Segment(seg_size);
    _segment_ring.emplace_back(seg);
  }
}

// 查询键对应的 Blob 内容（不执行惰性删除）
std::optional<Blob> Log::find(const Key& key) const {
  const auto& search = _index.find(key);
  if (search && !search.value()->is_expired(current_time_seconds())) {
    return search.value()->blob();
  }

  return std::nullopt;
}

// 查询键对应的完整 Value（带惰性过期检测与清理）
std::optional<Value> Log::find_value(const Key& key) {
  const auto search = _index.find(key);
  if (!search) {
    return std::nullopt;
  }

  auto* object = search.value();

  // 惰性过期检查：若对象已过期，则从索引中擦除并标记失效
  if (object->is_expired(current_time_seconds())) {
    // 仅当索引仍指向该键的当前对象时才删除，防止误删并发重写的新对象
    const auto current = _index.find(object->key());
    if (current && current.value() == object) {
      _index.erase(object->key());
    }
    object->expire();
    return std::nullopt;
  }

  return Value{object->blob(), object->flags(), object->expiration()};
}

// 追加写入键值对（若存储已满则循环回收旧段直到成功或不可回收）
bool Log::append(const Key& key, const Blob& blob, uint32_t flags, uint64_t expiration) {
  size_t object_size = Object::size_of(key, blob);

  // 对象尺寸不得超出单个段的上限
  if (object_size > _config.segment_size) {
    return false;
  }

  // 循环尝试追加写入；若空间不足则触发段淘汰回收
  for (;;) {
    if (try_to_append(key, blob, flags, expiration)) {
      return true;
    }

    if (expire(object_size) < object_size) {
      return false;
    }
  }
}

// 尝试在环形队列尾部段写入对象
bool Log::try_to_append(const Key& key, const Blob& blob, uint32_t flags, uint64_t expiration) {
  // 1. 优先尝试写入当前尾段
  if (try_to_append(_segment_ring[_segment_ring_tail], key, blob, flags, expiration)) {
    return true;
  }

  // 2. 当前尾段已满，推进到下一个尾段
  const auto next_tail = (_segment_ring_tail + 1) % _segment_ring.size();
  if (next_tail == _segment_ring_head) {
    // 无可用干净段，需进行淘汰
    return false;
  }

  _segment_ring_tail = next_tail;
  return try_to_append(_segment_ring[_segment_ring_tail], key, blob, flags, expiration);
}

// 在指定段内追加对象并原子更新哈希索引
bool Log::try_to_append(Segment* segment, const Key& key, const Blob& blob, uint32_t flags,
                        uint64_t expiration) {
  Object* object = segment->append(key, blob, flags, expiration);
  if (!object) {
    return false;
  }

  // 更新索引；若替换了旧对象，将旧对象标记为已失效
  if (auto old = _index.insert_or_assign(object->key(), object)) {
    old.value()->expire();
  }

  return true;
}

// 删除指定键
bool Log::remove(const Key& key) {
  auto value_opt = _index.find(key);
  if (!value_opt) {
    return false;
  }

  auto* object = value_opt.value();

  // 若对象已经逻辑过期，执行惰性清理后返回 false（未命中）
  if (object->is_expired(current_time_seconds())) {
    const auto current = _index.find(object->key());
    if (current && current.value() == object) {
      _index.erase(object->key());
    }
    object->expire();
    return false;
  }

  // 标记对象失效并从索引中移除
  object->expire();
  _index.erase(key);
  return true;
}

// 原子递增数值键
ArithmeticResult Log::incr(const Key& key, uint64_t delta) {
  return update_counter(key, delta, true);
}

// 原子递减数值键
ArithmeticResult Log::decr(const Key& key, uint64_t delta) {
  return update_counter(key, delta, false);
}

// 统一计数器更新逻辑（自增/自减计算与重新追加写入）
ArithmeticResult Log::update_counter(const Key& key, uint64_t delta, bool increment) {
  // 1. 查询当前键值
  auto current = find_value(key);
  if (!current) {
    return {ArithmeticStatus::NotFound, 0};
  }

  // 2. 解析十进制整数字符串
  auto parsed = parse_uint64_decimal(current->blob);
  if (!parsed) {
    return {ArithmeticStatus::NonNumeric, 0};
  }

  // 3. 执行模 2^64 算术运算（遵循 Memcached 协议规范）
  uint64_t updated;
  if (increment) {
    updated = parsed.value() + delta;
  } else {
    updated = parsed.value() < delta ? 0 : parsed.value() - delta;
  }

  // 4. 将新数值作为新对象追加写入日志内存
  auto encoded = std::to_string(updated);
  if (!append(key, encoded, current->flags, current->expiration)) {
    return {ArithmeticStatus::StorageFull, 0};
  }

  return {ArithmeticStatus::Success, updated};
}

// 淘汰头部最老的段，直至释放字节数满足回收目标
size_t Log::expire(size_t reclaim_target) {
  size_t nr_reclaimed = 0;

  while (_segment_ring_head != _segment_ring_tail) {
    nr_reclaimed += expire(_segment_ring[_segment_ring_head]);
    _segment_ring_head = (_segment_ring_head + 1) % _segment_ring.size();

    if (nr_reclaimed >= reclaim_target) {
      break;
    }
  }

  return nr_reclaimed;
}

// 淘汰指定段：遍历段内所有对象，从索引中注销引用，并重置该段
size_t Log::expire(Segment* segment) {
  Object* obj = segment->first_object();

  while (obj) {
    const auto current = _index.find(obj->key());
    if (current && current.value() == obj) {
      _index.erase(obj->key());
    }
    obj = segment->next_object(obj);
  }

  size_t nr_reclaimed = segment->size();
  segment->reset();

  return nr_reclaimed;
}

}  // namespace sphinx::logmem
