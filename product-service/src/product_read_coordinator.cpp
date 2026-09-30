// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_read_coordinator.h>

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sphinx {

struct ProductReadFlight {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<ProductLoadResult> result;
};

ProductReadTicket::ProductReadTicket(ProductReadCoordinator* owner, std::uint64_t id, ReadRole role,
                                     std::shared_ptr<ProductReadFlight> flight) noexcept
    : _owner{owner}, _id{id}, _role{role}, _flight{std::move(flight)} {}

ProductReadTicket::~ProductReadTicket() { abandon_if_needed(); }

ProductReadTicket::ProductReadTicket(ProductReadTicket&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)},
      _id{other._id},
      _role{other._role},
      _flight{std::move(other._flight)} {}

ProductReadTicket& ProductReadTicket::operator=(ProductReadTicket&& other) noexcept {
  if (this != &other) {
    abandon_if_needed();
    _owner = std::exchange(other._owner, nullptr);
    _id = other._id;
    _role = other._role;
    _flight = std::move(other._flight);
  }
  return *this;
}

ReadRole ProductReadTicket::role() const noexcept { return _role; }

std::uint64_t ProductReadTicket::id() const noexcept { return _id; }

ProductLoadResult ProductReadTicket::wait_until(
    std::chrono::steady_clock::time_point deadline) const {
  if (_role == ReadRole::Rejected) {
    return {ProductStatus::ReadBusy, std::nullopt};
  }
  if (!_flight) {
    return {ProductStatus::InternalError, std::nullopt};
  }

  std::unique_lock lock{_flight->mutex};
  const bool completed =
      _flight->ready.wait_until(lock, deadline, [this] { return _flight->result.has_value(); });
  if (!completed) {
    return {ProductStatus::ReadBusy, std::nullopt};
  }
  return _flight->result.value_or(ProductLoadResult{});
}

void ProductReadTicket::complete(ProductLoadResult result) noexcept {
  if (_owner == nullptr || _role != ReadRole::Leader || !_flight) {
    return;
  }
  ProductReadCoordinator* owner = std::exchange(_owner, nullptr);
  owner->complete(_id, _flight, std::move(result));
}

void ProductReadTicket::abandon_if_needed() noexcept {
  if (_owner != nullptr && _role == ReadRole::Leader && _flight) {
    ProductReadCoordinator* owner = std::exchange(_owner, nullptr);
    owner->abandon(_id, _flight);
  }
}

ProductLoadPermit::ProductLoadPermit(ProductReadCoordinator* owner) noexcept : _owner{owner} {}

ProductLoadPermit::~ProductLoadPermit() { release(); }

ProductLoadPermit::ProductLoadPermit(ProductLoadPermit&& other) noexcept
    : _owner{std::exchange(other._owner, nullptr)} {}

ProductLoadPermit& ProductLoadPermit::operator=(ProductLoadPermit&& other) noexcept {
  if (this != &other) {
    release();
    _owner = std::exchange(other._owner, nullptr);
  }
  return *this;
}

void ProductLoadPermit::release() noexcept {
  if (_owner != nullptr) {
    ProductReadCoordinator* owner = std::exchange(_owner, nullptr);
    owner->release_load();
  }
}

ProductReadCoordinator::ProductReadCoordinator(ProductReadOptions options) : _options{options} {
  if (_options.max_inflight_keys == 0 || _options.max_concurrent_loads == 0 ||
      _options.wait_timeout.count() <= 0) {
    throw std::invalid_argument{"invalid product read coordinator options"};
  }
}

ProductReadCoordinator::~ProductReadCoordinator() = default;

std::vector<ProductReadTicket> ProductReadCoordinator::acquire_many(
    const std::vector<std::uint64_t>& ids) {
  std::vector<ProductReadTicket> tickets;
  tickets.reserve(ids.size());
  for (const auto id : ids) {
    if (id == 0) {
      throw std::invalid_argument{"product read flight ID must be positive"};
    }
  }

  static_assert(std::is_nothrow_move_constructible_v<ProductReadTicket>);
  {
    std::lock_guard lock{_mutex};
    for (const auto id : ids) {
      const auto flight = _flights.find(id);
      if (flight != _flights.end()) {
        tickets.push_back(ProductReadTicket{this, id, ReadRole::Follower, flight->second});
        continue;
      }
      if (_flights.size() >= _options.max_inflight_keys) {
        tickets.push_back(ProductReadTicket{this, id, ReadRole::Rejected, nullptr});
        continue;
      }

      auto new_flight = std::make_shared<ProductReadFlight>();
      _flights.emplace(id, new_flight);
      tickets.push_back(ProductReadTicket{this, id, ReadRole::Leader, std::move(new_flight)});
    }
  }
  return tickets;
}

std::optional<ProductLoadPermit> ProductReadCoordinator::try_acquire_load() {
  std::lock_guard lock{_mutex};
  if (_active_loads >= _options.max_concurrent_loads) {
    return std::nullopt;
  }
  ++_active_loads;
  return ProductLoadPermit{this};
}

std::size_t ProductReadCoordinator::active_key_count() const {
  std::lock_guard lock{_mutex};
  return _flights.size();
}

std::size_t ProductReadCoordinator::active_load_count() const {
  std::lock_guard lock{_mutex};
  return _active_loads;
}

void ProductReadCoordinator::complete(std::uint64_t id,
                                      const std::shared_ptr<ProductReadFlight>& flight,
                                      ProductLoadResult result) noexcept {
  {
    std::lock_guard lock{flight->mutex};
    if (!flight->result) {
      flight->result.emplace(std::move(result));
    }
  }
  flight->ready.notify_all();

  std::lock_guard lock{_mutex};
  const auto position = _flights.find(id);
  if (position != _flights.end() && position->second == flight) {
    _flights.erase(position);
  }
}

void ProductReadCoordinator::abandon(std::uint64_t id,
                                     const std::shared_ptr<ProductReadFlight>& flight) noexcept {
  complete(id, flight, {ProductStatus::InternalError, std::nullopt});
}

void ProductReadCoordinator::release_load() noexcept {
  std::lock_guard lock{_mutex};
  if (_active_loads > 0) {
    --_active_loads;
  }
}

}  // namespace sphinx
