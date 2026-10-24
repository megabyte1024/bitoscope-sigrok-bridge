/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_DEVICE_SCAN_H
#define SIGROK_BRIDGE_DEVICE_SCAN_H

#include <optional>
#include <span>
#include <string>

#include <libsigrok/libsigrok.h>

#include "bitoscope_driver.h"

#include "usb_id_table.h"

namespace sigrok_bridge {

struct BridgeDevice;

struct OneDeviceScan {
  int status = BITOSCOPE_ERROR;
  std::string text;
  const DriverRow* row = nullptr;
  sr_dev_inst* sdi = nullptr;
  std::string sigrokConn;
  std::string portPath;
};

OneDeviceScan scanOneDevice(std::span<const DriverRow> table, const bitoscope_found_device& device);

OneDeviceScan scanDemoDevice();

std::string currentSigrokConn(sr_dev_inst* sdi);

std::optional<bool> foundAtItsPlace(const BridgeDevice& device);

}

#endif
