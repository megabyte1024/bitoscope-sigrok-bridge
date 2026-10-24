/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bridge_driver.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "device_entry.h"
#include "device_scan.h"
#include "sigrok_context.h"

namespace sigrok_bridge {

namespace {

constexpr const char* kDisplayName = "sigrok bridge";
constexpr const char* kVersion = "0.1.0";
constexpr const char* kDemoDeviceId = "demo-0";

Bridge& bridgeOf(void* user) { return *static_cast<Bridge*>(user); }

int enumerateDevices(void* user, [[maybe_unused]] const bitoscope_connection* connection,
                     bitoscope_found_devices* found) {
  return bridgeOf(user).enumerate(found);
}

int identifyDevice(void* user, const bitoscope_found_device* found, bitoscope_device* device,
                   bitoscope_reply* reply) {
  return bridgeOf(user).identify(*found, device, reply);
}

int openForSession(void* user, const bitoscope_found_device* found, [[maybe_unused]] bitoscope_device* device,
                   [[maybe_unused]] bitoscope_progress* progress, void** session, bitoscope_reply* reply) {
  return bridgeOf(user).open(*found, session, reply);
}

void closeAtSessionEnd(void* user, void* session) { bridgeOf(user).close(session); }

}

Bridge::Bridge(std::span<const DriverRow> table, DemoMode demo) : table_(table), demo_(demo) {
  if (demo_ == DemoMode::kOff) {
    for (const DeclaredUsbId& id : declaredUsbIds(table_)) {
      usbIds_.push_back({id.vendorId, id.productId, id.interfaceNumber, id.genericName});
    }
  }
}

Bridge::~Bridge() = default;

bitoscope_driver Bridge::description() {
  bitoscope_driver driver{};
  driver.display_name = kDisplayName;
  driver.version = kVersion;
  driver.user = this;
  driver.identify = identifyDevice;
  driver.open = openForSession;
  driver.close = closeAtSessionEnd;
  if (demo_ == DemoMode::kOff) {
    driver.transport = BITOSCOPE_TRANSPORT_USB;
    driver.usb_ids = usbIds_.data();
    driver.num_usb_ids = usbIds_.size();
  } else {
    driver.transport = BITOSCOPE_TRANSPORT_NONE;
    driver.enumerate = enumerateDevices;
  }
  return driver;
}

int Bridge::enumerate(bitoscope_found_devices* found) {
  static bool said = false;
  if (!said) {
    said = true;
    std::string value;
    readEnvironment("BITOSCOPE_SIGROK_BRIDGE_DEMO", value);
    bitoscope_log(BITOSCOPE_LOG_WARNING,
                  "BITOSCOPE_SIGROK_BRIDGE_DEMO=%s: libsigrok's demo device is listed, and no real device is looked "
                  "for",
                  value.c_str());
  }
  return bitoscope_add_found_device(found, kDemoDeviceId);
}

namespace {

int answer(bitoscope_reply* reply, int status, const std::string& text) {
  if (status != BITOSCOPE_OK && !text.empty()) {
    bitoscope_reply_message(reply, text.c_str());
  }
  return status;
}

void takeScan(BridgeDevice& device, const bitoscope_found_device& found, const OneDeviceScan& scan) {
  device.row = scan.row;
  device.sdi = scan.sdi;
  device.driver = sr_dev_inst_driver_get(scan.sdi);
  device.id = found.id;
  device.sigrokConn = scan.sigrokConn;
  device.portPath = scan.portPath;
}

}

int Bridge::identify(const bitoscope_found_device& found, bitoscope_device* device, bitoscope_reply* reply) {
  std::lock_guard lock(sigrokMutex());
  const OneDeviceScan scan = demo_ == DemoMode::kOff ? scanOneDevice(table_, found) : scanDemoDevice();
  if (scan.status != BITOSCOPE_OK) {
    return answer(reply, scan.status, scan.text);
  }
  BridgeDevice described;
  takeScan(described, found, scan);
  std::string whyNot;
  return answer(reply, describeDevice(described, device, whyNot), whyNot);
}

int Bridge::open(const bitoscope_found_device& found, void** session, bitoscope_reply* reply) {
  std::lock_guard lock(sigrokMutex());
  if (held_ != nullptr) {
    return answer(reply, BITOSCOPE_ERROR, "sigrok bridge holds " + held_->id + " already");
  }
  const OneDeviceScan scan = demo_ == DemoMode::kOff ? scanOneDevice(table_, found) : scanDemoDevice();
  if (scan.status != BITOSCOPE_OK) {
    return answer(reply, scan.status, scan.text);
  }
  auto opened = std::make_unique<BridgeDevice>();
  takeScan(*opened, found, scan);
  std::string whyNot;
  if (const int status = describeDevice(*opened, nullptr, whyNot); status != BITOSCOPE_OK) {
    return answer(reply, status, whyNot);
  }
  clearLastSigrokError();
  if (openDevice(*opened) != SR_OK) {
    const std::string sigrokError = lastSigrokError();
    const std::string text = "libsigrok could not open " + opened->id + (sigrokError.empty() ? "" : ": " + sigrokError);
    return answer(reply, claimRefusedAsBusy(sigrokError) ? BITOSCOPE_BUSY : BITOSCOPE_ERROR, text);
  }
  if (const std::string conn = currentSigrokConn(opened->sdi); !conn.empty()) {
    opened->sigrokConn = conn;
  }
  bitoscope_log(BITOSCOPE_LOG_INFO, "%s is open and held until session ends", opened->id.c_str());
  held_ = opened.release();
  *session = held_;
  return BITOSCOPE_OK;
}

void Bridge::close(void* session) {
  std::lock_guard lock(sigrokMutex());
  auto* device = static_cast<BridgeDevice*>(session);
  if (device == held_) {
    held_ = nullptr;
  }
  if (device->capturing.load()) {
    // WHY: An abandoned libsigrok session may still call into the device, so it is never freed.
    bitoscope_log(BITOSCOPE_LOG_WARNING, "%s is left open: capture's session was abandoned on it", device->id.c_str());
    return;
  }
  closeDevice(*device);
  delete device;
}

BridgeDevice* Bridge::heldDevice() { return held_; }

DemoMode demoModeOfEnvironment() {
  std::string text;
  return demoModeFromText(readEnvironment("BITOSCOPE_SIGROK_BRIDGE_DEMO", text) ? text.c_str() : nullptr);
}

}
