/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_LISTING_RULES_H
#define SIGROK_BRIDGE_LISTING_RULES_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "usb_id_table.h"

namespace sigrok_bridge {

std::string usbPortPath(int bus, std::span<const uint8_t> ports);

std::vector<const DriverRow*> rowsDeclaring(std::span<const DriverRow> table, uint16_t vendorId, uint16_t productId);

struct DeclaredUsbId {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  const char* genericName = nullptr;
};
std::vector<DeclaredUsbId> declaredUsbIds(std::span<const DriverRow> table);

std::string sigrokConn(uint8_t bus, uint8_t address);

bool claimRefusedAsBusy(std::string_view sigrokError);

std::string ownSerialNumber(std::string_view serial, std::string_view vendor, std::string_view model);

std::vector<uint64_t> sampleRateSeries125Hz(uint64_t lowHz, uint64_t highHz);

std::vector<uint64_t> hzToMilliHz(std::span<const uint64_t> ratesHz);

std::vector<uint64_t> rateCeilingsMilliHz(const DriverRow* row, uint32_t numChannels,
                                          std::span<const uint64_t> ratesMilliHz);

bool logicChannelIndicesValid(std::span<const int> indices);

enum class DemoMode { kOff, kOnly };
DemoMode demoModeFromText(const char* text);

int sigrokLogLevelFromText(const char* text);

enum class LogSeverity { kDebug, kInfo, kWarning, kError };
LogSeverity severityOfSigrokLevel(int sigrokLevel);
bool isProblemLevel(int sigrokLevel);

std::string firmwareDirectoryChoice(bool userSetIt, const std::string& besideExecutable,
                                    bool besideExecutableExists, const std::string& systemDirectory,
                                    bool systemDirectoryExists);

}

#endif
