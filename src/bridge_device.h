/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_BRIDGE_DEVICE_H
#define SIGROK_BRIDGE_BRIDGE_DEVICE_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <libsigrok/libsigrok.h>

#include "pwm_generator.h"
#include "threshold.h"
#include "usb_id_table.h"

namespace sigrok_bridge {

struct BridgeDevice {
  const DriverRow* row = nullptr;
  sr_dev_driver* driver = nullptr;
  sr_dev_inst* sdi = nullptr;
  std::string id;
  std::string name;
  std::vector<uint64_t> sampleRatesMilliHz;
  std::vector<std::string> channelNames;
  std::string serialNumber;
  uint64_t defaultSampleRateMilliHz = 0;
  std::string sigrokConn;
  std::string portPath;
  ThresholdSetting threshold;
  PwmGenerator pwm;
  bool isOpen = false;
  std::atomic<bool> capturing{false};
};

int openDevice(BridgeDevice& device);

void closeDevice(BridgeDevice& device);

}

#endif
