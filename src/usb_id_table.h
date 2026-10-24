/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_USB_ID_TABLE_H
#define SIGROK_BRIDGE_USB_ID_TABLE_H

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sigrok_bridge {

struct UsbId {
  uint16_t vendorId;
  uint16_t productId;
};

struct DriverRow {
  const char* driver;
  std::span<const UsbId> usbIds = {};
  const char* genericName = nullptr;
  uint8_t usbInterface = 0;
  bool honoursConn = true;
  uint64_t (*rateCeilingHz)(uint32_t selectedChannels, uint32_t totalChannels) = nullptr;
  const char* (*startRefusal)(uint32_t channelMask, uint64_t rateHz) = nullptr;
  uint64_t pwmMinFrequencyHz = 0;
  uint64_t pwmMaxFrequencyHz = 0;
  bool endsAtSampleLimit = false;
  uint8_t sampleEndpoint = 0;
  bool keepCoreAwake = false;
  const char* firmwareUploadedLine = nullptr;
};

struct ShimEndpoint {
  uint16_t vendorId;
  uint16_t productId;
  uint8_t endpoint;
  bool operator==(const ShimEndpoint&) const = default;
};

std::vector<ShimEndpoint> shimEndpoints(std::span<const DriverRow> rows);

std::span<const DriverRow> driverTable();

const DriverRow* findDriverRow(std::string_view driver);

const DriverRow& demoDriverRow();

}

#endif
