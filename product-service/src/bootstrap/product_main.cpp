// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
#include <sphinx/product/backends/mysql/mysql_runtime.h>
#include <sphinx/product/bootstrap/product_config.h>
#include <sphinx/product/bootstrap/product_worker.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <thread>

namespace {

bool block_shutdown_signals(sigset_t* wait_set, sigset_t* previous_mask) noexcept {
  if (sigemptyset(wait_set) != 0 || sigaddset(wait_set, SIGINT) != 0 ||
      sigaddset(wait_set, SIGTERM) != 0 || sigaddset(wait_set, SIGUSR1) != 0) {
    return false;
  }
  return pthread_sigmask(SIG_BLOCK, wait_set, previous_mask) == 0;
}

bool wake_control_thread(std::thread& control_thread) noexcept {
  // SIGUSR1 只用于通知控制线程：serve() 已结束，可以退出等待。
  // NOLINTNEXTLINE(bugprone-bad-signal-to-kill-thread)
  const int thread_result = pthread_kill(control_thread.native_handle(), SIGUSR1);
  if (thread_result == 0 || thread_result == ESRCH) {
    return true;
  }
  if (kill(getpid(), SIGUSR1) == 0) {
    return false;
  }
  (void)pthread_cancel(control_thread.native_handle());
  return false;
}

bool serve_until_shutdown(sphinx::ProductHttpServer& server, const sigset_t& shutdown_signals) {
  std::atomic<bool> runtime_failed{false};
  std::thread control_thread{[&] {
    int received_signal = 0;
    if (sigwait(&shutdown_signals, &received_signal) != 0) {
      runtime_failed.store(true, std::memory_order_release);
      server.stop();
    } else if (received_signal != SIGUSR1) {
      server.stop();
    }
  }};

  bool serve_result = false;
  try {
    serve_result = server.serve();
  } catch (...) {
    runtime_failed.store(true, std::memory_order_release);
  }

  const bool control_woken = wake_control_thread(control_thread);
  control_thread.join();
  return serve_result && !runtime_failed.load(std::memory_order_acquire) && control_woken;
}

}  // namespace

int main() {
  sigset_t shutdown_signals;
  sigset_t previous_mask;
  if (!block_shutdown_signals(&shutdown_signals, &previous_mask)) {
    std::cerr << "product service signal setup failed\n";
    return 1;
  }

  try {
    const auto config = sphinx::load_product_config();
    // server 最先析构并等待 Worker 退出，随后销毁共享状态与 MySQL 客户端运行时。
    sphinx::MySqlRuntime mysql_runtime;
    sphinx::ProductSharedState shared{config.read_options, config.breaker_options};
    sphinx::ProductHttpServer server{config.http,
                                     sphinx::make_product_worker_factory(config, shared), shared};
    const bool served = serve_until_shutdown(server, shutdown_signals);
    const int restore_result = pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    if (restore_result != 0) {
      std::cerr << "product service signal restore failed\n";
      return 1;
    }
    if (!served) {
      std::cerr << "product service bind, listen or runtime failed\n";
      return 1;
    }
    return 0;
  } catch (...) {
    (void)pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    std::cerr << "product service startup failed\n";
    return 1;
  }
}
