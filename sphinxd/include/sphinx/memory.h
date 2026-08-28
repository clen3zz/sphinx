// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

namespace sphinx::memory {

class Memory
{
  void* _addr;
  size_t _size;

public:
  static Memory mmap(size_t size);
  explicit Memory(void* ptr, size_t size);
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;
  Memory(Memory&& other) noexcept;
  Memory& operator=(Memory&& other) noexcept;
  ~Memory();
  void* addr() const;
  size_t size() const;

private:
  void release() noexcept;
};
} // namespace sphinx::memory
