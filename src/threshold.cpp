/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "threshold.h"

#include <cmath>
#include <cstdio>

#include <libsigrok/libsigrok.h>

#include "bridge_device.h"

namespace sigrok_bridge {

namespace {

constexpr uint32_t maxDigits = 3;
constexpr double maxMagnitude = 1e9;
constexpr double epsilon = 1e-9;
constexpr double wholeUnitTolerance = 1e-6;

const char* const thresholdId = "threshold";

double powerOfTen(uint32_t digits) {
  double power = 1.0;
  for (uint32_t i = 0; i < digits; ++i) {
    power *= 10.0;
  }
  return power;
}

bool usable(double volts) { return std::isfinite(volts) && std::fabs(volts) <= maxMagnitude; }

uint32_t digitsFor(double volts) {
  for (uint32_t d = 0; d < maxDigits; ++d) {
    const double scaled = std::fabs(volts) * powerOfTen(d);
    if (std::fabs(scaled - std::round(scaled)) < wholeUnitTolerance) {
      return d;
    }
  }
  return maxDigits;
}

int64_t toFixed(double volts, uint32_t digits) {
  return static_cast<int64_t>(std::llround(volts * powerOfTen(digits)));
}

std::string formatVolts(double volts) {
  char text[64];
  std::snprintf(text, sizeof(text), "%.3f", volts);
  std::string result = text;
  while (!result.empty() && result.back() == '0') {
    result.pop_back();
  }
  if (!result.empty() && result.back() == '.') {
    result.pop_back();
  }
  return result == "-0" ? "0" : result;
}

bool describeCustomRange(GVariant* list, GVariant* current, ThresholdDescription& out) {
  if (!g_variant_is_of_type(list, G_VARIANT_TYPE("ad")) || g_variant_n_children(list) != 3) {
    return false;
  }
  double values[3] = {0, 0, 0};
  for (gsize i = 0; i < 3; ++i) {
    GVariant* child = g_variant_get_child_value(list, i);
    values[i] = g_variant_get_double(child);
    g_variant_unref(child);
  }
  const double minimum = values[0];
  const double maximum = values[1];
  const double step = values[2];
  if (!usable(minimum) || !usable(maximum) || !usable(step) || !(step > 0.0) || minimum > maximum) {
    return false;
  }
  const uint32_t digits = digitsFor(step);
  const double power = powerOfTen(digits);
  const int64_t low = static_cast<int64_t>(std::ceil(minimum * power - wholeUnitTolerance));
  const int64_t high = static_cast<int64_t>(std::floor(maximum * power + wholeUnitTolerance));
  if (low > high) {
    return false;
  }
  double initial = (minimum + maximum) / 2.0;
  if (current != nullptr && g_variant_is_of_type(current, G_VARIANT_TYPE_DOUBLE)) {
    const double read = g_variant_get_double(current);
    if (std::isfinite(read)) {
      initial = read;
    }
  }
  int64_t initialFixed = toFixed(initial, digits);
  if (initialFixed < low) {
    initialFixed = low;
  }
  if (initialFixed > high) {
    initialFixed = high;
  }
  out.setting.kind = ThresholdKind::customRange;
  out.setting.digits = digits;
  out.type = BITOSCOPE_PROPERTY_DECIMAL;
  out.digits = digits;
  out.minimum = low;
  out.maximum = high;
  out.defaultValue = initialFixed;
  return true;
}

bool describeVoltageTuples(GVariant* list, GVariant* current, ThresholdDescription& out) {
  if (!g_variant_is_of_type(list, G_VARIANT_TYPE("a(dd)")) || g_variant_n_children(list) == 0) {
    return false;
  }
  std::vector<std::pair<double, double>> tuples;
  bool allEqual = true;
  const gsize count = g_variant_n_children(list);
  for (gsize i = 0; i < count; ++i) {
    GVariant* child = g_variant_get_child_value(list, i);
    double low = 0.0;
    double high = 0.0;
    g_variant_get(child, "(dd)", &low, &high);
    g_variant_unref(child);
    if (!usable(low) || !usable(high)) {
      return false;
    }
    tuples.emplace_back(low, high);
    allEqual = allEqual && low == high;
  }
  uint32_t defaultIndex = 0;
  if (current != nullptr && g_variant_is_of_type(current, G_VARIANT_TYPE("(dd)"))) {
    double low = 0.0;
    double high = 0.0;
    g_variant_get(current, "(dd)", &low, &high);
    for (size_t i = 0; i < tuples.size(); ++i) {
      if (std::fabs(tuples[i].first - low) < epsilon && std::fabs(tuples[i].second - high) < epsilon) {
        defaultIndex = static_cast<uint32_t>(i);
        break;
      }
    }
  }
  out.setting.kind = ThresholdKind::voltageTuples;
  out.type = BITOSCOPE_PROPERTY_ENUMERATION;
  out.defaultIndex = defaultIndex;
  if (allEqual) {
    uint32_t digits = 0;
    for (const auto& tuple : tuples) {
      const uint32_t needed = digitsFor(tuple.first);
      digits = needed > digits ? needed : digits;
    }
    out.digits = digits;
    for (const auto& tuple : tuples) {
      out.itemNumbers.push_back(toFixed(tuple.first, digits));
    }
  } else {
    for (const auto& tuple : tuples) {
      out.itemNames.push_back(formatVolts(tuple.first) + " V / " + formatVolts(tuple.second) + " V");
    }
  }
  out.setting.tuples = tuples;
  return true;
}

bool describePresetNames(GVariant* list, GVariant* current, ThresholdDescription& out) {
  if (!g_variant_is_of_type(list, G_VARIANT_TYPE("as")) || g_variant_n_children(list) == 0) {
    return false;
  }
  std::vector<std::string> names;
  const gsize count = g_variant_n_children(list);
  for (gsize i = 0; i < count; ++i) {
    GVariant* child = g_variant_get_child_value(list, i);
    names.emplace_back(g_variant_get_string(child, nullptr));
    g_variant_unref(child);
  }
  uint32_t defaultIndex = 0;
  if (current != nullptr && g_variant_is_of_type(current, G_VARIANT_TYPE_STRING)) {
    const char* name = g_variant_get_string(current, nullptr);
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) {
        defaultIndex = static_cast<uint32_t>(i);
        break;
      }
    }
  }
  out.setting.kind = ThresholdKind::presetNames;
  out.type = BITOSCOPE_PROPERTY_ENUMERATION;
  out.itemNames = names;
  out.setting.names = names;
  out.defaultIndex = defaultIndex;
  return true;
}

bool optionsAllow(const BridgeDevice& device, uint32_t key) {
  const uint32_t capabilities =
      static_cast<uint32_t>(sr_dev_config_capabilities_list(device.sdi, nullptr, static_cast<int>(key)));
  const uint32_t needed = static_cast<uint32_t>(SR_CONF_SET) | static_cast<uint32_t>(SR_CONF_LIST);
  return (capabilities & needed) == needed;
}

bool queryThreshold(const BridgeDevice& device, ThresholdKind kind, ThresholdDescription& out) {
  const uint32_t key = thresholdKey(kind);
  if (!optionsAllow(device, key)) {
    return false;
  }
  const sr_key_info* info = sr_key_info_get(SR_KEY_CONFIG, key);
  const char* keyName = info != nullptr ? info->id : "?";
  GVariant* list = nullptr;
  if (sr_config_list(device.driver, device.sdi, nullptr, key, &list) != SR_OK || list == nullptr) {
    bitoscope_log(BITOSCOPE_LOG_INFO, "%s: no threshold from %s: its list needs the device open",
                  device.id.c_str(), keyName);
    return false;
  }
  GVariant* current = nullptr;
  if (sr_config_get(device.driver, device.sdi, nullptr, key, &current) != SR_OK) {
    current = nullptr;
  }
  const bool described = describeThreshold(kind, list, current, out);
  if (current != nullptr) {
    g_variant_unref(current);
  }
  g_variant_unref(list);
  if (!described) {
    bitoscope_log(BITOSCOPE_LOG_INFO, "%s: no threshold from %s: its list is not usable", device.id.c_str(),
                  keyName);
  }
  return described;
}

}

uint32_t thresholdKey(ThresholdKind kind) {
  switch (kind) {
    case ThresholdKind::customRange:
      return SR_CONF_LOGIC_THRESHOLD_CUSTOM;
    case ThresholdKind::voltageTuples:
      return SR_CONF_VOLTAGE_THRESHOLD;
    case ThresholdKind::presetNames:
      return SR_CONF_LOGIC_THRESHOLD;
    case ThresholdKind::none:
      break;
  }
  return 0;
}

bool describeThreshold(ThresholdKind kind, GVariant* list, GVariant* current, ThresholdDescription& out) {
  out = ThresholdDescription();
  if (list == nullptr) {
    return false;
  }
  bool ok = false;
  switch (kind) {
    case ThresholdKind::customRange:
      ok = describeCustomRange(list, current, out);
      break;
    case ThresholdKind::voltageTuples:
      ok = describeVoltageTuples(list, current, out);
      break;
    case ThresholdKind::presetNames:
      ok = describePresetNames(list, current, out);
      break;
    case ThresholdKind::none:
      break;
  }
  if (!ok) {
    out = ThresholdDescription();
  }
  return ok;
}

GVariant* thresholdVariant(const ThresholdSetting& setting, const bitoscope_capture& capture) {
  switch (setting.kind) {
    case ThresholdKind::customRange:
      return g_variant_new_double(static_cast<double>(bitoscope_property_integer(&capture, thresholdId)) /
                                  powerOfTen(setting.digits));
    case ThresholdKind::voltageTuples: {
      const uint32_t index = bitoscope_property_enum_index(&capture, thresholdId);
      if (index >= setting.tuples.size()) {
        return nullptr;
      }
      const auto& tuple = setting.tuples[index];
      GVariant* range[2] = {g_variant_new_double(tuple.first), g_variant_new_double(tuple.second)};
      return g_variant_new_tuple(range, 2);
    }
    case ThresholdKind::presetNames: {
      const uint32_t index = bitoscope_property_enum_index(&capture, thresholdId);
      if (index >= setting.names.size()) {
        return nullptr;
      }
      return g_variant_new_string(setting.names[index].c_str());
    }
    case ThresholdKind::none:
      break;
  }
  return nullptr;
}

bool findThreshold(BridgeDevice& device, ThresholdDescription& description) {
  device.threshold = ThresholdSetting();
  if (device.driver == nullptr || device.sdi == nullptr) {
    return false;
  }
  for (ThresholdKind kind :
       {ThresholdKind::customRange, ThresholdKind::voltageTuples, ThresholdKind::presetNames}) {
    if (queryThreshold(device, kind, description)) {
      device.threshold = description.setting;
      return true;
    }
  }
  return false;
}

bitoscope_property thresholdProperty(const ThresholdDescription& description, std::vector<const char*>& namePointers) {
  namePointers.clear();
  for (const std::string& name : description.itemNames) {
    namePointers.push_back(name.c_str());
  }
  bitoscope_property property{};
  property.id = thresholdId;
  property.name = "Threshold";
  property.type = description.type;
  property.help = "Input voltage above which sample reads as high.";
  property.group = "Input";
  property.unit = BITOSCOPE_UNIT_VOLT;
  property.changeable_during_capture = 0;
  property.digits = description.digits;
  if (description.type == BITOSCOPE_PROPERTY_DECIMAL) {
    property.has_minimum = 1;
    property.minimum = description.minimum;
    property.has_maximum = 1;
    property.maximum = description.maximum;
    property.default_value = description.defaultValue;
  } else if (!description.itemNames.empty()) {
    property.item_names = namePointers.data();
    property.num_items = namePointers.size();
    property.default_index = description.defaultIndex;
  } else {
    property.item_numbers = description.itemNumbers.data();
    property.num_items = description.itemNumbers.size();
    property.default_index = description.defaultIndex;
  }
  return property;
}

int applyThreshold(BridgeDevice& device, const bitoscope_capture& capture) {
  if (device.threshold.kind == ThresholdKind::none) {
    return SR_OK;
  }
  GVariant* variant = thresholdVariant(device.threshold, capture);
  if (variant == nullptr) {
    return SR_ERR_ARG;
  }
  return sr_config_set(device.sdi, nullptr, thresholdKey(device.threshold.kind), variant);
}

}
