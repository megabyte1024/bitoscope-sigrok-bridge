/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usb_id_table.h"

#include <algorithm>

namespace sigrok_bridge {

namespace {

constexpr uint64_t kMHz = 1000000;

constexpr UsbId kFx2lafwIds[] = {
    {0x08a9, 0x0014}, {0x08a9, 0x0015}, {0x08a9, 0x0009}, {0x08a9, 0x0005}, {0x0925, 0x3881},
    {0x04b4, 0x8613}, {0x16d0, 0x0498}, {0x1d50, 0x608c}, {0x1d50, 0x608d}, {0x1d50, 0x608f},
};
constexpr UsbId kKingstLa2016Ids[] = {{0x77a1, 0x01a2}};
constexpr UsbId kSaleaeLogic16Ids[] = {{0x21a9, 0x1001}};
constexpr UsbId kSaleaeLogicProIds[] = {{0x21a9, 0x1006}};
constexpr UsbId kDslogicIds[] = {
    {0x2a0e, 0x0001}, {0x2a0e, 0x0002}, {0x2a0e, 0x0003}, {0x2a0e, 0x0020}, {0x2a0e, 0x0021},
};
constexpr UsbId kGreatfetIds[] = {{0x1d50, 0x60e6}};

constexpr uint64_t kFx2lafwWideMaxHz = 12 * kMHz;
constexpr uint32_t kFx2lafwNarrowChannelCount = 8;
constexpr uint32_t kFx2lafwChannels8To15Mask = 0xff00u;
constexpr uint64_t kKingstLa2016PwmMaxHz = 20 * kMHz;

uint64_t fx2lafwCeilingHz(uint32_t selectedChannels, uint32_t totalChannels) {
  (void)totalChannels;
  return selectedChannels > kFx2lafwNarrowChannelCount ? kFx2lafwWideMaxHz : 0;
}

const char* fx2lafwStartRefusal(uint32_t channelMask, uint64_t rateHz) {
  if ((channelMask & kFx2lafwChannels8To15Mask) != 0 && rateHz > kFx2lafwWideMaxHz) {
    return "fx2lafw: a capture that includes any of channels 8-15 cannot run above 12 MHz";
  }
  return nullptr;
}

constexpr uint8_t kFx2lafwSampleEndpoint = 0x82;
constexpr uint8_t kKingstLa2016SampleEndpoint = 0x86;
constexpr uint8_t kSaleaeLogic16SampleEndpoint = 0x82;
constexpr uint8_t kSaleaeLogicProSampleEndpoint = 0x82;
constexpr uint8_t kDslogicSampleEndpoint = 0x86;

constexpr DriverRow kRows[] = {
    {.driver = "fx2lafw",
     .usbIds = kFx2lafwIds,
     .genericName = "FX2 logic analyzer (sigrok)",
     .usbInterface = 0,
     .rateCeilingHz = fx2lafwCeilingHz,
     .startRefusal = fx2lafwStartRefusal,
     .endsAtSampleLimit = false,
     .sampleEndpoint = kFx2lafwSampleEndpoint,
     .keepCoreAwake = true},
    {.driver = "kingst-la2016",
     .usbIds = kKingstLa2016Ids,
     .genericName = "Kingst logic analyzer (sigrok)",
     .usbInterface = 0,
     .pwmMinFrequencyHz = 1,
     .pwmMaxFrequencyHz = kKingstLa2016PwmMaxHz,
     .endsAtSampleLimit = false,
     .sampleEndpoint = kKingstLa2016SampleEndpoint},
    {.driver = "saleae-logic16",
     .usbIds = kSaleaeLogic16Ids,
     .genericName = "Saleae Logic16 (sigrok)",
     .usbInterface = 0,
     .endsAtSampleLimit = false,
     .sampleEndpoint = kSaleaeLogic16SampleEndpoint},
    // WHY: Its scan loads firmware into every Logic Pro without it before conn applies: all of them enumerate again.
    {.driver = "saleae-logic-pro",
     .usbIds = kSaleaeLogicProIds,
     .genericName = "Saleae Logic Pro 16 (sigrok)",
     .usbInterface = 0,
     .endsAtSampleLimit = false,
     .sampleEndpoint = kSaleaeLogicProSampleEndpoint,
     .firmwareUploadedLine = "saleae-logic-pro: Firmware upload done."},
    {.driver = "dreamsourcelab-dslogic",
     .usbIds = kDslogicIds,
     .genericName = "DreamSourceLab DSLogic (sigrok)",
     .usbInterface = 0,
     .endsAtSampleLimit = false,
     .sampleEndpoint = kDslogicSampleEndpoint},
    // WHY: Its scan with conn takes whatever device sits at that address: scan it for these USB ids only.
    {.driver = "greatfet", .usbIds = kGreatfetIds, .genericName = "GreatFET (sigrok)", .usbInterface = 1},
};

}

std::span<const DriverRow> driverTable() { return kRows; }

std::vector<ShimEndpoint> shimEndpoints(std::span<const DriverRow> rows) {
  std::vector<ShimEndpoint> endpoints;
  for (const DriverRow& row : rows) {
    if (row.sampleEndpoint == 0) {
      continue;
    }
    for (const UsbId& id : row.usbIds) {
      endpoints.push_back({id.vendorId, id.productId, row.sampleEndpoint});
    }
  }
  return endpoints;
}

const DriverRow* findDriverRow(std::string_view driver) {
  for (const DriverRow& row : driverTable()) {
    if (driver == row.driver) {
      return &row;
    }
  }
  return nullptr;
}

const DriverRow& demoDriverRow() {
  static const DriverRow row{.driver = "demo", .endsAtSampleLimit = true};
  return row;
}

}
