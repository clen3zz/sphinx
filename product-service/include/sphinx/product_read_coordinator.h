// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sphinx/product.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sphinx {

struct ProductReadOptions {
  std::size_t max_inflight_keys = 1024;
  std::size_t max_concurrent_loads = 2;
  std::chrono::milliseconds wait_timeout{500};
};

class ProductReadCoordinator;
struct ProductReadFlight;

enum class ReadRole : std::uint8_t { Leader, Follower, Rejected };

class ProductReadTicket final {
 public:
  ProductReadTicket() = default;
  ~ProductReadTicket();
  ProductReadTicket(const ProductReadTicket&) = delete;
  ProductReadTicket& operator=(const ProductReadTicket&) = delete;
  ProductReadTicket(ProductReadTicket&& other) noexcept;
  ProductReadTicket& operator=(ProductReadTicket&& other) noexcept;

  ReadRole role() const noexcept;
  ProductLoadResult wait_until(std::chrono::steady_clock::time_point deadline) const;
  bool wait_timed_out() const noexcept;
  void complete(ProductLoadResult result) noexcept;

 private:
  ProductReadTicket(ProductReadCoordinator* owner, std::uint64_t id, ReadRole role,
                    std::shared_ptr<ProductReadFlight> flight) noexcept;

  ProductReadCoordinator* _owner = nullptr;
  std::uint64_t _id = 0;
  ReadRole _role = ReadRole::Rejected;
  std::shared_ptr<ProductReadFlight> _flight;
  // Lets metrics distinguish a follower deadline from ReadBusy published by its leader.
  mutable bool _wait_timed_out = false;

  friend class ProductReadCoordinator;
};

class ProductLoadPermit final {
 public:
  ProductLoadPermit() = default;
  ~ProductLoadPermit();
  ProductLoadPermit(const ProductLoadPermit&) = delete;
  ProductLoadPermit& operator=(const ProductLoadPermit&) = delete;
  ProductLoadPermit(ProductLoadPermit&& other) noexcept;
  ProductLoadPermit& operator=(ProductLoadPermit&& other) noexcept;

 private:
  explicit ProductLoadPermit(ProductReadCoordinator* owner) noexcept;
  void release() noexcept;

  ProductReadCoordinator* _owner = nullptr;

  friend class ProductReadCoordinator;
};

class ProductReadCoordinator final {
 public:
  explicit ProductReadCoordinator(ProductReadOptions options);
  ~ProductReadCoordinator();
  ProductReadCoordinator(const ProductReadCoordinator&) = delete;
  ProductReadCoordinator& operator=(const ProductReadCoordinator&) = delete;
  ProductReadCoordinator(ProductReadCoordinator&&) = delete;
  ProductReadCoordinator& operator=(ProductReadCoordinator&&) = delete;

  std::vector<ProductReadTicket> acquire_many(const std::vector<std::uint64_t>& ids);
  std::optional<ProductLoadPermit> try_acquire_load();
  std::size_t active_key_count() const;
  std::size_t active_load_count() const;
  std::chrono::milliseconds wait_timeout() const noexcept;

 private:
  void complete(std::uint64_t id, const std::shared_ptr<ProductReadFlight>& flight,
                ProductLoadResult result) noexcept;
  void release_load() noexcept;

  ProductReadOptions _options;
  mutable std::mutex _mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<ProductReadFlight>> _flights;
  std::size_t _active_loads = 0;

  friend class ProductReadTicket;
  friend class ProductLoadPermit;
};

}  // namespace sphinx
