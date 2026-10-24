/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core_keeper.h"

#include <climits>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

namespace sigrok_bridge {

namespace {

constexpr int kProcessorWaitsBetweenStopChecks = 4096;

inline void waitForTheProcessor() {
#ifdef _WIN32
  YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

}

CoreKeeper::CoreKeeper(bool enabled) {
  if (!enabled) {
    return;
  }
  thread_ = std::thread([this] {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
#else
    sched_param parameter{};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &parameter);
#endif
    while (!stop_.load(std::memory_order_relaxed)) {
      for (int i = 0; i < kProcessorWaitsBetweenStopChecks; ++i) {
        waitForTheProcessor();
      }
    }
  });
}

CoreKeeper::~CoreKeeper() {
  stop_.store(true, std::memory_order_relaxed);
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool CoreKeeper::running() const noexcept { return thread_.joinable(); }

#ifdef _WIN32
uint64_t CoreKeeper::cpuTimeMs() const noexcept {
  if (!thread_.joinable()) {
    return 0;
  }
  constexpr uint64_t kHundredNanosecondsPerMillisecond = 10000;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(const_cast<std::thread&>(thread_).native_handle(), &created, &exited, &kernel, &user)) {
    return 0;
  }
  const auto hundredNanoseconds = [](const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return (hundredNanoseconds(kernel) + hundredNanoseconds(user)) / kHundredNanosecondsPerMillisecond;
}

int CoreKeeper::priority() const noexcept {
  if (!thread_.joinable()) {
    return INT_MIN;
  }
  return GetThreadPriority(const_cast<std::thread&>(thread_).native_handle());
}
#else
uint64_t CoreKeeper::cpuTimeMs() const noexcept {
  if (!thread_.joinable()) {
    return 0;
  }
  constexpr uint64_t kMillisecondsPerSecond = 1000;
  constexpr uint64_t kNanosecondsPerMillisecond = 1000000;
  clockid_t clock;
  if (pthread_getcpuclockid(const_cast<std::thread&>(thread_).native_handle(), &clock) != 0) {
    return 0;
  }
  timespec used{};
  if (clock_gettime(clock, &used) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(used.tv_sec) * kMillisecondsPerSecond +
         static_cast<uint64_t>(used.tv_nsec) / kNanosecondsPerMillisecond;
}

int CoreKeeper::priority() const noexcept {
  if (!thread_.joinable()) {
    return INT_MIN;
  }
  int policy = 0;
  sched_param parameter{};
  if (pthread_getschedparam(const_cast<std::thread&>(thread_).native_handle(), &policy, &parameter) != 0) {
    return INT_MIN;
  }
  return policy;
}
#endif

}
