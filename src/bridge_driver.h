/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_BRIDGE_DRIVER_H
#define SIGROK_BRIDGE_BRIDGE_DRIVER_H

#include <span>
#include <string>
#include <vector>

#include "bitoscope_driver.h"

#include "bridge_device.h"
#include "listing_rules.h"
#include "usb_id_table.h"

namespace sigrok_bridge {

class Bridge {
 public:
  Bridge(std::span<const DriverRow> table, DemoMode demo);
  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;
  ~Bridge();

  bitoscope_driver description();

  int enumerate(bitoscope_found_devices* found);
  int identify(const bitoscope_found_device& found, bitoscope_device* device, bitoscope_reply* reply);
  int open(const bitoscope_found_device& found, void** session, bitoscope_reply* reply);
  void close(void* session);

  BridgeDevice* heldDevice();

 private:
  std::span<const DriverRow> table_;
  DemoMode demo_;
  std::vector<bitoscope_usb_id> usbIds_;
  BridgeDevice* held_ = nullptr;
};

DemoMode demoModeOfEnvironment();

}

#endif
