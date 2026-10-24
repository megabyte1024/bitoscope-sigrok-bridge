/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_SIGROK_CONTEXT_H
#define SIGROK_BRIDGE_SIGROK_CONTEXT_H

#include <mutex>
#include <string>

#include <libsigrok/libsigrok.h>

namespace sigrok_bridge {

std::recursive_mutex& sigrokMutex();

sr_context* sigrokContext();

bool setUpUsbShim();
bool usbRawIoOffForTest();

bool readEnvironment(const char* name, std::string& value);

std::string lastSigrokError();
void clearLastSigrokError();

class SigrokLineWatch {
 public:
  explicit SigrokLineWatch(const char* line);
  ~SigrokLineWatch();
  SigrokLineWatch(const SigrokLineWatch&) = delete;
  SigrokLineWatch& operator=(const SigrokLineWatch&) = delete;

  bool seen() const;

 private:
  bool watching_ = false;
  int previousLevel_ = 0;
};

}

#endif
