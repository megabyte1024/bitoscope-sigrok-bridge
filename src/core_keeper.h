/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_CORE_KEEPER_H
#define SIGROK_BRIDGE_CORE_KEEPER_H

#include <atomic>
#include <cstdint>
#include <thread>

namespace sigrok_bridge {

class CoreKeeper {
 public:
  explicit CoreKeeper(bool enabled);
  ~CoreKeeper();
  CoreKeeper(const CoreKeeper&) = delete;
  CoreKeeper& operator=(const CoreKeeper&) = delete;

  bool running() const noexcept;
  uint64_t cpuTimeMs() const noexcept;
  int priority() const noexcept;

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}

#endif
