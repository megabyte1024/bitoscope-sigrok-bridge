/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "device_scan.h"

#include <cstdio>
#include <set>
#include <string_view>

#include <glib.h>

#include "bridge_device.h"
#include "listing_rules.h"
#include "sigrok_context.h"

namespace sigrok_bridge {

namespace {

std::set<std::string>& initializedDrivers() {
  static std::set<std::string> names;
  return names;
}

sr_dev_driver* sigrokDriver(const char* name, std::string& why) {
  sr_context* context = sigrokContext();
  if (context == nullptr) {
    why = "libsigrok did not start";
    return nullptr;
  }
  sr_dev_driver* driver = nullptr;
  for (sr_dev_driver** drivers = sr_driver_list(context); drivers != nullptr && *drivers != nullptr; ++drivers) {
    if (std::string_view((*drivers)->name) == name) {
      driver = *drivers;
      break;
    }
  }
  if (driver == nullptr) {
    why = std::string("this libsigrok has no driver ") + name;
    return nullptr;
  }
  if (initializedDrivers().count(name) == 0) {
    if (sr_driver_init(context, driver) != SR_OK) {
      why = std::string("libsigrok driver ") + name + " does not initialize";
      return nullptr;
    }
    initializedDrivers().insert(name);
  }
  return driver;
}

GSList* scanDriver(sr_dev_driver* driver, const std::string* conn) {
  if (conn == nullptr) {
    return sr_driver_scan(driver, nullptr);
  }
  sr_config option{};
  option.key = SR_CONF_CONN;
  option.data = g_variant_ref_sink(g_variant_new_string(conn->c_str()));
  GSList* options = g_slist_append(nullptr, &option);
  GSList* found = sr_driver_scan(driver, options);
  g_slist_free(options);
  g_variant_unref(option.data);
  return found;
}

sr_dev_inst* instanceAt(GSList* found, const std::string& portPath) {
  for (GSList* l = found; l != nullptr; l = l->next) {
    auto* sdi = static_cast<sr_dev_inst*>(l->data);
    const char* connection = sr_dev_inst_connid_get(sdi);
    if (connection != nullptr && portPath == connection) {
      return sdi;
    }
  }
  return nullptr;
}

enum class Address { notListed, readable, unreadable };

Address addressOf(sr_dev_inst* sdi, std::string& conn) {
  const int capabilities = sr_dev_config_capabilities_list(sdi, nullptr, SR_CONF_CONN);
  if ((static_cast<uint32_t>(capabilities) & static_cast<uint32_t>(SR_CONF_GET)) == 0) {
    return Address::notListed;
  }
  GVariant* value = nullptr;
  if (sr_config_get(sr_dev_inst_driver_get(sdi), sdi, nullptr, SR_CONF_CONN, &value) != SR_OK || value == nullptr) {
    return Address::unreadable;
  }
  conn = g_variant_get_string(value, nullptr);
  g_variant_unref(value);
  return Address::readable;
}

std::string firmwareLoadingText(const DriverRow& row) {
  return std::string(row.driver) + " loaded firmware into it, and it enumerates again";
}

std::string usbIdText(uint16_t vendorId, uint16_t productId) {
  char text[16];
  std::snprintf(text, sizeof(text), "%04x:%04x", vendorId, productId);
  return text;
}

}

OneDeviceScan scanOneDevice(std::span<const DriverRow> table, const bitoscope_found_device& device) {
  OneDeviceScan result;
  const std::string id = device.id != nullptr ? device.id : "";
  if (device.transport != BITOSCOPE_TRANSPORT_USB || device.usb == nullptr) {
    result.text = "device \"" + id + "\" was not found on USB";
    return result;
  }
  const bitoscope_usb_location& usb = *device.usb;
  const std::string conn = sigrokConn(usb.bus, usb.address);
  const std::string portPath = usbPortPath(usb.bus, std::span<const uint8_t>(usb.ports, usb.num_ports));
  std::string tried;
  for (const DriverRow* row : rowsDeclaring(table, usb.vendor_id, usb.product_id)) {
    tried += (tried.empty() ? "" : ", ") + std::string(row->driver);
    std::string why;
    sr_dev_driver* driver = sigrokDriver(row->driver, why);
    if (driver == nullptr) {
      bitoscope_log(BITOSCOPE_LOG_WARNING, "%s: %s", id.c_str(), why.c_str());
      continue;
    }
    // WHY: Clear before each scan: a scan adds its instances to the driver's list, and nothing else frees them.
    sr_dev_clear(driver);
    const SigrokLineWatch firmwareUpload(row->firmwareUploadedLine);
    GSList* found = scanDriver(driver, row->honoursConn ? &conn : nullptr);
    sr_dev_inst* sdi = instanceAt(found, portPath);
    g_slist_free(found);
    if (sdi == nullptr) {
      if (firmwareUpload.seen()) {
        result.status = BITOSCOPE_FIRMWARE_LOADING;
        result.text = firmwareLoadingText(*row);
        result.row = row;
        return result;
      }
      continue;
    }
    result.row = row;
    result.sdi = sdi;
    result.sigrokConn = conn;
    result.portPath = portPath;
    std::string current;
    switch (addressOf(sdi, current)) {
      case Address::unreadable:
        result.status = BITOSCOPE_FIRMWARE_LOADING;
        result.text = firmwareLoadingText(*row);
        return result;
      case Address::readable:
        result.sigrokConn = current;
        break;
      case Address::notListed:
        break;
    }
    result.status = BITOSCOPE_OK;
    return result;
  }
  result.text = "libsigrok found no device at " + conn + " (USB id " + usbIdText(usb.vendor_id, usb.product_id) +
                ", driver " + (tried.empty() ? std::string("none") : tried) + ")";
  return result;
}

OneDeviceScan scanDemoDevice() {
  OneDeviceScan result;
  const DriverRow& row = demoDriverRow();
  std::string why;
  sr_dev_driver* driver = sigrokDriver(row.driver, why);
  if (driver == nullptr) {
    result.text = why;
    return result;
  }
  sr_dev_clear(driver);
  GSList* found = sr_driver_scan(driver, nullptr);
  result.sdi = found != nullptr ? static_cast<sr_dev_inst*>(found->data) : nullptr;
  g_slist_free(found);
  if (result.sdi == nullptr) {
    result.text = "libsigrok's demo driver found no device";
    return result;
  }
  result.row = &row;
  result.status = BITOSCOPE_OK;
  return result;
}

std::string currentSigrokConn(sr_dev_inst* sdi) {
  std::string conn;
  return addressOf(sdi, conn) == Address::readable ? conn : std::string();
}

std::optional<bool> foundAtItsPlace(const BridgeDevice& device) {
  if (device.row == nullptr || device.sigrokConn.empty()) {
    return std::nullopt;
  }
  std::string why;
  sr_dev_driver* driver = sigrokDriver(device.row->driver, why);
  if (driver == nullptr) {
    bitoscope_log(BITOSCOPE_LOG_WARNING, "%s: %s", device.id.c_str(), why.c_str());
    return std::nullopt;
  }
  // WHY: No sr_dev_clear here: it would free the held instance; this scan's instances stay until the next clear.
  GSList* found = scanDriver(driver, device.row->honoursConn ? &device.sigrokConn : nullptr);
  const bool present = instanceAt(found, device.portPath) != nullptr;
  g_slist_free(found);
  return present;
}

}
