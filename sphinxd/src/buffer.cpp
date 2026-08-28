// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/buffer.h>

#include <stdexcept>
#include <string_view>
#include <vector>

namespace sphinx {

// 检查缓冲区是否为空
bool Buffer::is_empty() const {
  return _data.empty();
}

// 向缓冲区尾部追加数据
void Buffer::append(std::string_view data) {
  // 空数据直接返回，避免无谓的扩容开销
  if (data.empty()) {
    return;
  }

  _data.insert(_data.end(), data.data(), data.data() + data.size());
}

// 移除缓冲区前缀指定长度的数据
void Buffer::remove_prefix(size_t n) {
  // 边界校验：移除长度不得超过当前缓冲区大小
  if (n > _data.size()) {
    throw std::out_of_range("buffer prefix is larger than the buffer");
  }

  // 计算迭代器偏移并擦除前缀数据
  using DifferenceType = std::vector<char>::difference_type;
  _data.erase(_data.begin(), _data.begin() + static_cast<DifferenceType>(n));
}

// 获取缓冲区底层原始数据指针
const char* Buffer::data() const {
  return _data.data();
}

// 获取当前缓冲区已存储的字节数
size_t Buffer::size() const {
  return _data.size();
}

// 构造并返回当前缓冲区内容的只读 string_view 视图
std::string_view Buffer::string_view() const {
  return std::string_view{_data.data(), _data.size()};
}

}  // namespace sphinx
