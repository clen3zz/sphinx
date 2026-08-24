/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <optional>
#include <unordered_map>

namespace sphinx::index {

template<typename Key, typename Value>
class Index
{
  std::unordered_map<Key, Value> _index;

public:
  std::optional<Value> find(Key key) const
  {
    auto it = _index.find(key);
    if (it != _index.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  std::optional<Value> insert_or_assign(Key key, Value value)
  {
    auto it = _index.find(key);
    if (it == _index.end()) {
      _index.emplace(key, value);
      return std::nullopt;
    }
    auto old = it->second;
    // `Key` is allowed to be a non-owning view (the log uses string_view keys).
    // insert_or_assign updates only the mapped value when a key compares equal,
    // leaving the old view pointing into an object that may be reclaimed.  Erase
    // and insert so the index key is rebound to the newly appended object.
    _index.erase(it);
    _index.emplace(key, value);
    return old;
  }
  void erase(Key key)
  {
    _index.erase(key);
  }
};
}
