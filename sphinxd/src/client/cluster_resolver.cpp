// SPDX-License-Identifier: Apache-2.0
#include "cluster_resolver.h"

#include <sphinx/cluster_client.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sphinx {
namespace {

[[noreturn]] void throw_node_error(std::string_view target, std::string_view detail) {
  throw ClientError{std::string{"node "} + std::string{target} + ": " + std::string{detail}};
}

}  // namespace

struct ResolveState {
  std::mutex mutex;
  std::condition_variable ready;
  AddressList addresses{nullptr, &freeaddrinfo};
  int status = EAI_AGAIN;
  bool complete = false;
};

// getaddrinfo cannot be interrupted portably. Detach at most 32 resolver workers; each owns its
// result until it finishes, so a caller can return at its deadline without a dangling pointer.
AddressList resolve_with_deadline(const std::string& host, const std::string& port,
                                  std::chrono::steady_clock::time_point deadline,
                                  std::string_view target) {
  static auto active = std::make_shared<std::atomic<size_t>>(0);
  auto active_counter = active;
  constexpr size_t max_active_resolvers = 32;
  auto state = std::make_shared<ResolveState>();
  auto count = active->load(std::memory_order_relaxed);
  while (true) {
    if (count >= max_active_resolvers) {
      throw_node_error(target, "resolver is busy");
    }
    if (active->compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) {
      break;
    }
  }

  try {
    std::thread{[state, active_counter, host, port] {
      addrinfo hints = {};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;
      addrinfo* addresses = nullptr;
      const auto status = getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
      {
        std::scoped_lock const lock{state->mutex};
        state->addresses.reset(addresses);
        state->status = status;
        state->complete = true;
      }
      state->ready.notify_one();
      active_counter->fetch_sub(1, std::memory_order_release);
    }}.detach();
  } catch (...) {
    active->fetch_sub(1, std::memory_order_release);
    throw;
  }

  std::unique_lock lock{state->mutex};
  if (!state->ready.wait_until(lock, deadline, [&] { return state->complete; })) {
    throw_node_error(target, "operation timed out");
  }
  if (state->status != 0) {
    throw_node_error(target, std::string{"cannot resolve host: "} + gai_strerror(state->status));
  }
  lock.unlock();
  return std::move(state->addresses);
}

}  // namespace sphinx
