/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "listing_rules.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace sigrok_bridge {

std::string usbPortPath(int bus, std::span<const uint8_t> ports) {
  if (ports.empty()) {
    return {};
  }
  std::string path = "usb/" + std::to_string(bus) + "-" + std::to_string(ports[0]);
  for (size_t i = 1; i < ports.size(); ++i) {
    path += "." + std::to_string(ports[i]);
  }
  return path;
}

std::vector<const DriverRow*> rowsDeclaring(std::span<const DriverRow> table, uint16_t vendorId, uint16_t productId) {
  std::vector<const DriverRow*> rows;
  for (const DriverRow& row : table) {
    const bool declares = std::any_of(row.usbIds.begin(), row.usbIds.end(), [&](const UsbId& id) {
      return id.vendorId == vendorId && id.productId == productId;
    });
    if (declares) {
      rows.push_back(&row);
    }
  }
  return rows;
}

std::vector<DeclaredUsbId> declaredUsbIds(std::span<const DriverRow> table) {
  std::vector<DeclaredUsbId> declared;
  for (const DriverRow& row : table) {
    for (const UsbId& id : row.usbIds) {
      const bool seen = std::any_of(declared.begin(), declared.end(), [&](const DeclaredUsbId& one) {
        return one.vendorId == id.vendorId && one.productId == id.productId;
      });
      if (!seen) {
        declared.push_back({id.vendorId, id.productId, row.usbInterface, row.genericName});
      }
    }
  }
  return declared;
}

std::string sigrokConn(uint8_t bus, uint8_t address) { return std::to_string(bus) + "." + std::to_string(address); }

bool claimRefusedAsBusy(std::string_view sigrokError) {
  constexpr std::string_view kTexts[] = {"Another program or driver", "LIBUSB_ERROR_BUSY"};
  return std::any_of(std::begin(kTexts), std::end(kTexts),
                     [&](std::string_view text) { return sigrokError.find(text) != std::string_view::npos; });
}

namespace {

std::string comparableName(std::string_view text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}

std::string ownSerialNumber(std::string_view serial, std::string_view vendor, std::string_view model) {
  const std::string compared = comparableName(serial);
  if (compared.empty()) {
    return {};
  }
  const std::string vendorName = comparableName(vendor);
  const std::string modelName = comparableName(model);
  const std::string names[] = {vendorName, modelName, comparableName(std::string(vendor) + " " + std::string(model))};
  const bool repeatsName =
      std::any_of(std::begin(names), std::end(names), [&](const std::string& name) { return name == compared; });
  return repeatsName ? std::string() : std::string(serial);
}

std::vector<uint64_t> sampleRateSeries125Hz(uint64_t lowHz, uint64_t highHz) {
  static constexpr uint64_t kMantissas[] = {1, 2, 5};
  std::vector<uint64_t> rates;
  for (uint64_t decade = 1;; decade *= 10) {
    for (const uint64_t mantissa : kMantissas) {
      if (decade > UINT64_MAX / mantissa) {
        return rates;
      }
      const uint64_t rate = decade * mantissa;
      if (rate > highHz) {
        return rates;
      }
      if (rate >= lowHz) {
        rates.push_back(rate);
      }
    }
    if (decade > UINT64_MAX / 10) {
      return rates;
    }
  }
}

std::vector<uint64_t> hzToMilliHz(std::span<const uint64_t> ratesHz) {
  std::vector<uint64_t> milli;
  milli.reserve(ratesHz.size());
  for (const uint64_t rate : ratesHz) {
    milli.push_back(rate * 1000);
  }
  return milli;
}

std::vector<uint64_t> rateCeilingsMilliHz(const DriverRow* row, uint32_t numChannels,
                                          std::span<const uint64_t> ratesMilliHz) {
  std::vector<uint64_t> ceilings;
  if (row == nullptr || row->rateCeilingHz == nullptr || numChannels == 0 || ratesMilliHz.empty()) {
    return ceilings;
  }
  const auto [lowest, highest] = std::minmax_element(ratesMilliHz.begin(), ratesMilliHz.end());
  ceilings.reserve(numChannels);
  for (uint32_t selected = 1; selected <= numChannels; ++selected) {
    const uint64_t limitHz = row->rateCeilingHz(selected, numChannels);
    const uint64_t limit = limitHz == 0 ? *highest : limitHz * 1000;
    ceilings.push_back(std::clamp(limit, *lowest, *highest));
  }
  return ceilings;
}

bool logicChannelIndicesValid(std::span<const int> indices) {
  if (indices.empty() || indices.size() > 32) {
    return false;
  }
  uint64_t seen = 0;
  for (const int index : indices) {
    if (index < 0 || static_cast<size_t>(index) >= indices.size() || (seen & (1ull << index)) != 0) {
      return false;
    }
    seen |= 1ull << index;
  }
  return true;
}

DemoMode demoModeFromText(const char* text) {
  const std::string_view value = text == nullptr ? std::string_view() : std::string_view(text);
  return value == "1" || value == "only" ? DemoMode::kOnly : DemoMode::kOff;
}

int sigrokLogLevelFromText(const char* text) {
  constexpr int kWarnings = 2;
  const std::string_view value = text == nullptr ? std::string_view() : std::string_view(text);
  if (value.size() == 1 && value[0] >= '0' && value[0] <= '5') {
    return value[0] - '0';
  }
  return kWarnings;
}

LogSeverity severityOfSigrokLevel(int sigrokLevel) {
  if (sigrokLevel <= 1) {
    return LogSeverity::kError;
  }
  if (sigrokLevel == 2) {
    return LogSeverity::kWarning;
  }
  if (sigrokLevel == 3) {
    return LogSeverity::kInfo;
  }
  return LogSeverity::kDebug;
}

bool isProblemLevel(int sigrokLevel) { return sigrokLevel >= 1 && sigrokLevel <= 2; }

std::string firmwareDirectoryChoice(bool userSetIt, const std::string& besideExecutable,
                                    bool besideExecutableExists, const std::string& systemDirectory,
                                    bool systemDirectoryExists) {
  if (userSetIt) {
    return {};
  }
  if (besideExecutableExists) {
    return besideExecutable;
  }
  if (systemDirectoryExists) {
    return systemDirectory;
  }
  return {};
}

}
