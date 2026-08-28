// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <unordered_map>

namespace sphinx::index {

template <typename Key, typename Value>
class Index {
  std::unordered_map<Key, Value> _index;

 public:
  std::optional<Value> find(const Key& key) const {
    auto it = _index.find(key);
    if (it != _index.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  std::optional<Value> insert_or_assign(const Key& key, Value value) {
    auto it = _index.find(key);
    if (it == _index.end()) {
      _index.emplace(key, value);
      return std::nullopt;
    }
    auto old = it->second;
    // `Key` 可以是非拥有视图（日志使用 string_view 作为键）。
    // 当键比较相等时，insert_or_assign 只更新映射值，
    // 可能使旧视图仍指向已被回收的对象。因此先删除再插入，
    // 让索引键重新绑定到刚追加的对象。
    _index.erase(it);
    _index.emplace(key, value);
    return old;
  }
  void erase(const Key& key) {
    _index.erase(key);
  }
};
}  // namespace sphinx::index
