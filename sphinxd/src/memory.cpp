// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/memory.h>
#include <sys/mman.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace sphinx {

// 通过 mmap 匿名映射分配大块内存（启用 MAP_POPULATE 预先填充页表，减少缺页异常）
Memory Memory::mmap(size_t size) {
  void* addr =
      ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_POPULATE, -1, 0);

  // 检查映射是否成功，失败则抛出包含具体 errno 的系统错误
  if (addr == MAP_FAILED) {
    throw std::system_error(errno, std::system_category(), "mmap");
  }

  return Memory{addr, size};
}

// 构造函数：接管指定地址与大小的已映射内存
Memory::Memory(void* ptr, size_t size) : _addr{ptr}, _size{size} {
}

// 移动构造函数：转移内存所有权，并将源对象重置为空
Memory::Memory(Memory&& other) noexcept
    : _addr{std::exchange(other._addr, nullptr)}, _size{std::exchange(other._size, 0)} {
}

// 移动赋值操作符：释放旧资源并接管源对象的内存所有权
Memory& Memory::operator=(Memory&& other) noexcept {
  if (this != &other) {
    // 释放自身当前持有的内存映射
    release();

    // 窃取源对象资源并将其重置
    _addr = std::exchange(other._addr, nullptr);
    _size = std::exchange(other._size, 0);
  }

  return *this;
}

// 获取映射内存的起始地址
void* Memory::addr() const {
  return _addr;
}

// 获取映射内存的总字节大小
size_t Memory::size() const {
  return _size;
}

// 析构函数：RAII 自动解映射并释放内存资源
Memory::~Memory() {
  release();
}

// 释放内存映射（系统调用 munmap）并清空内部状态
void Memory::release() noexcept {
  // 仅在有效映射时执行解映射操作
  if (_addr != nullptr && _size != 0) {
    ::munmap(_addr, _size);
  }

  _addr = nullptr;
  _size = 0;
}

}  // namespace sphinx
