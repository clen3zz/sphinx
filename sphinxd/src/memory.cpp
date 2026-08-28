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

#include <sphinx/memory.h>

#include <system_error>
#include <utility>

#include <sys/mman.h>

namespace sphinx::memory {

Memory
Memory::mmap(size_t size)
{
  void* addr =
    ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_POPULATE, -1, 0);
  if (addr == MAP_FAILED) {
    throw std::system_error(errno, std::system_category(), "mmap");
  }
  return Memory{addr, size};
}

Memory::Memory(void* addr, size_t size)
  : _addr{addr}
  , _size{size}
{
}

Memory::Memory(Memory&& other) noexcept
  : _addr{std::exchange(other._addr, nullptr)}
  , _size{std::exchange(other._size, 0)}
{
}

Memory&
Memory::operator=(Memory&& other) noexcept
{
  if (this != &other) {
    release();
    _addr = std::exchange(other._addr, nullptr);
    _size = std::exchange(other._size, 0);
  }
  return *this;
}

void*
Memory::addr() const
{
  return _addr;
}

size_t
Memory::size() const
{
  return _size;
}

Memory::~Memory()
{
  release();
}

void
Memory::release() noexcept
{
  if (_addr != nullptr && _size != 0) {
    ::munmap(_addr, _size);
  }
  _addr = nullptr;
  _size = 0;
}
}
