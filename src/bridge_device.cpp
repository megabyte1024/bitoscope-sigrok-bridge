/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bridge_device.h"

#include "sigrok_context.h"

namespace sigrok_bridge {

int openDevice(BridgeDevice& device) {
  std::lock_guard lock(sigrokMutex());
  if (device.isOpen) {
    return SR_OK;
  }
  const int status = sr_dev_open(device.sdi);
  device.isOpen = status == SR_OK;
  return status;
}

void closeDevice(BridgeDevice& device) {
  std::lock_guard lock(sigrokMutex());
  if (device.isOpen) {
    sr_dev_close(device.sdi);
    device.isOpen = false;
  }
}

}
