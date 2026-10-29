/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_THRESHOLD_H
#define SIGROK_BRIDGE_THRESHOLD_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>

#include "bitoscope_driver.h"

namespace sigrok_bridge {

struct BridgeDevice;

enum class ThresholdKind {
  none,
  customRange,
  voltageTuples,
  presetNames
};

struct ThresholdSetting {
  ThresholdKind kind = ThresholdKind::none;
  uint32_t digits = 0;
  std::vector<std::pair<double, double>> tuples;
  std::vector<std::string> names;
};

struct ThresholdDescription {
  ThresholdSetting setting;
  int type = BITOSCOPE_PROPERTY_DECIMAL;
  uint32_t digits = 0;
  int64_t minimum = 0;
  int64_t maximum = 0;
  int64_t defaultValue = 0;
  std::vector<std::string> itemNames;
  std::vector<int64_t> itemNumbers;
  uint32_t defaultIndex = 0;
};

uint32_t thresholdKey(ThresholdKind kind);

bool describeThreshold(ThresholdKind kind, GVariant* list, GVariant* current, ThresholdDescription& out);

GVariant* thresholdVariant(const ThresholdSetting& setting, const bitoscope_capture& capture);

bool findThreshold(BridgeDevice& device, ThresholdDescription& description);

bitoscope_property thresholdProperty(const ThresholdDescription& description, std::vector<const char*>& namePointers);

int applyThreshold(BridgeDevice& device, const bitoscope_capture& capture);

}

#endif
