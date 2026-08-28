// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <sphinx/memory.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <system_error>
#include <utility>

namespace sphinx::memory {

Memory Memory::mmap(size_t size) {
  void* addr =
      ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_POPULATE, -1, 0);
  if (addr == MAP_FAILED) {
    throw std::system_error(errno, std::system_category(), "mmap");
  }
  return Memory{addr, size};
}

Memory::Memory(void* addr, size_t size) : _addr{addr}, _size{size} {
}

Memory::Memory(Memory&& other) noexcept
    : _addr{std::exchange(other._addr, nullptr)}, _size{std::exchange(other._size, 0)} {
}

Memory& Memory::operator=(Memory&& other) noexcept {
  if (this != &other) {
    release();
    _addr = std::exchange(other._addr, nullptr);
    _size = std::exchange(other._size, 0);
  }
  return *this;
}

void* Memory::addr() const {
  return _addr;
}

size_t Memory::size() const {
  return _size;
}

Memory::~Memory() {
  release();
}

void Memory::release() noexcept {
  if (_addr != nullptr && _size != 0) {
    ::munmap(_addr, _size);
  }
  _addr = nullptr;
  _size = 0;
}
}  // namespace sphinx::memory
