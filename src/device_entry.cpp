/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "device_entry.h"

#include <algorithm>
#include <string>
#include <vector>

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include "bridge_device.h"
#include "capture.h"
#include "listing_rules.h"
#include "pwm_generator.h"
#include "threshold.h"
#include "usb_id_table.h"

namespace sigrok_bridge {

namespace {

bool optionsContain(const sr_dev_driver* driver, const sr_dev_inst* sdi, uint32_t key) {
  bool found = false;
  GArray* options = sr_dev_options(driver, sdi, nullptr);
  if (options != nullptr) {
    for (guint i = 0; i < options->len; ++i) {
      if (g_array_index(options, uint32_t, i) == key) {
        found = true;
        break;
      }
    }
    g_array_free(options, TRUE);
  }
  return found;
}

bool hasOption(const sr_dev_driver* driver, const sr_dev_inst* sdi, uint32_t key) {
  return optionsContain(driver, nullptr, key) || optionsContain(driver, sdi, key);
}

bool listSampleRatesHz(const sr_dev_driver* driver, const sr_dev_inst* sdi, std::vector<uint64_t>& ratesHz) {
  ratesHz.clear();
  GVariant* list = nullptr;
  if (sr_config_list(driver, sdi, nullptr, SR_CONF_SAMPLERATE, &list) != SR_OK || list == nullptr) {
    return false;
  }
  gsize count = 0;
  GVariant* rates = g_variant_lookup_value(list, "samplerates", G_VARIANT_TYPE("at"));
  if (rates != nullptr) {
    const auto* values = static_cast<const uint64_t*>(g_variant_get_fixed_array(rates, &count, sizeof(uint64_t)));
    ratesHz.assign(values, values + count);
    g_variant_unref(rates);
  } else {
    GVariant* steps = g_variant_lookup_value(list, "samplerate-steps", G_VARIANT_TYPE("at"));
    if (steps != nullptr) {
      const auto* values =
          static_cast<const uint64_t*>(g_variant_get_fixed_array(steps, &count, sizeof(uint64_t)));
      if (count == 3) {
        ratesHz = sampleRateSeries125Hz(values[0], values[1]);
      }
      g_variant_unref(steps);
    }
  }
  g_variant_unref(list);
  std::sort(ratesHz.begin(), ratesHz.end());
  ratesHz.erase(std::unique(ratesHz.begin(), ratesHz.end()), ratesHz.end());
  ratesHz.erase(std::remove(ratesHz.begin(), ratesHz.end(), uint64_t{0}), ratesHz.end());
  return !ratesHz.empty();
}

uint64_t currentSampleRateHz(const sr_dev_driver* driver, const sr_dev_inst* sdi) {
  GVariant* value = nullptr;
  uint64_t rate = 0;
  if (sr_config_get(driver, sdi, nullptr, SR_CONF_SAMPLERATE, &value) == SR_OK && value != nullptr) {
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64)) {
      rate = g_variant_get_uint64(value);
    }
    g_variant_unref(value);
  }
  return rate;
}

std::string nameOf(const sr_dev_inst* sdi, bool demo, const char* driverName) {
  if (demo) {
    return "Demo device (sigrok)";
  }
  const char* vendor = sr_dev_inst_vendor_get(sdi);
  const char* model = sr_dev_inst_model_get(sdi);
  std::string name;
  if (vendor != nullptr && vendor[0] != '\0') {
    name = vendor;
  }
  if (model != nullptr && model[0] != '\0') {
    name += name.empty() ? "" : " ";
    name += model;
  }
  if (name.empty()) {
    name = driverName;
  }
  return name + " (sigrok)";
}

bool listAnalyzer(bitoscope_device* listed, BridgeDevice& device, std::string& whyNot) {
  if (bitoscope_set_name(listed, device.name.c_str()) != BITOSCOPE_OK) {
    whyNot = "SDK refused device " + device.id;
    return false;
  }
  if (!device.serialNumber.empty()) {
    bitoscope_set_serial_number(listed, device.serialNumber.c_str());
  }

  const uint32_t numChannels = static_cast<uint32_t>(device.channelNames.size());
  std::vector<bitoscope_channel_preset> presets;
  for (uint32_t i = 0; i < numChannels; ++i) {
    bitoscope_channel_preset preset{};
    preset.channel = i;
    preset.name = device.channelNames[i].c_str();
    presets.push_back(preset);
  }
  const std::vector<uint64_t> ceilings = rateCeilingsMilliHz(device.row, numChannels, device.sampleRatesMilliHz);

  bitoscope_logic_analyzer analyzer{};
  analyzer.num_channels = numChannels;
  analyzer.sample_rates_milli_hz = device.sampleRatesMilliHz.data();
  analyzer.num_sample_rates = device.sampleRatesMilliHz.size();
  analyzer.capture = captureSamples;
  analyzer.prepare = prepareCapture;
  analyzer.rate_ceilings_milli_hz = ceilings.empty() ? nullptr : ceilings.data();
  analyzer.num_rate_ceilings = ceilings.size();
  analyzer.default_sample_rate_milli_hz = device.defaultSampleRateMilliHz;
  analyzer.channel_presets = presets.data();
  analyzer.num_channel_presets = presets.size();
  if (bitoscope_add_logic_analyzer(listed, &analyzer) != BITOSCOPE_OK) {
    whyNot = "SDK refused logic analyzer of " + device.id;
    return false;
  }
  return true;
}

bool addToEntry(bitoscope_device* listed, BridgeDevice& device, std::string& whyNot) {
  if (listed != nullptr && !listAnalyzer(listed, device, whyNot)) {
    return false;
  }
  addThresholdProperty(listed, device);
  addPwmGenerator(listed, device);
  return true;
}

}

int describeDevice(BridgeDevice& device, bitoscope_device* listed, std::string& whyNot) {
  const sr_dev_driver* driver = device.driver;
  const sr_dev_inst* sdi = device.sdi;
  const bool demo = device.row == &demoDriverRow();

  if (!hasOption(driver, sdi, SR_CONF_LOGIC_ANALYZER)) {
    whyNot = "libsigrok does not list it as logic analyzer (no SR_CONF_LOGIC_ANALYZER)";
    return BITOSCOPE_NOT_SUPPORTED;
  }
  if (!hasOption(driver, sdi, SR_CONF_CONTINUOUS)) {
    whyNot = "it cannot stream (no SR_CONF_CONTINUOUS)";
    return BITOSCOPE_NOT_SUPPORTED;
  }

  std::vector<sr_channel*> channels;
  std::vector<int> indices;
  for (GSList* l = sr_dev_inst_channels_get(sdi); l != nullptr; l = l->next) {
    auto* channel = static_cast<sr_channel*>(l->data);
    if (channel->type == SR_CHANNEL_LOGIC) {
      channels.push_back(channel);
      indices.push_back(channel->index);
    }
  }
  if (!logicChannelIndicesValid(indices)) {
    whyNot = "its logic channels are not numbered 0 to n-1 with n from 1 to 32";
    return BITOSCOPE_NOT_SUPPORTED;
  }
  std::sort(channels.begin(), channels.end(),
            [](const sr_channel* a, const sr_channel* b) { return a->index < b->index; });

  std::vector<uint64_t> ratesHz;
  if (!listSampleRatesHz(driver, sdi, ratesHz)) {
    whyNot = "libsigrok lists no sample rates for it without opening it";
    return BITOSCOPE_NOT_SUPPORTED;
  }
  const std::vector<uint64_t> ratesMilliHz = hzToMilliHz(ratesHz);
  const uint64_t currentMilliHz = currentSampleRateHz(driver, sdi) * 1000;
  const bool currentListed = std::binary_search(ratesMilliHz.begin(), ratesMilliHz.end(), currentMilliHz);

  device.name = nameOf(sdi, demo, driver->name);
  device.channelNames.clear();
  for (const sr_channel* channel : channels) {
    device.channelNames.push_back(channel->name != nullptr ? channel->name : "");
  }
  device.sampleRatesMilliHz = ratesMilliHz;
  device.defaultSampleRateMilliHz = currentListed ? currentMilliHz : 0;
  const char* serial = demo ? nullptr : sr_dev_inst_sernum_get(sdi);
  const char* vendor = sr_dev_inst_vendor_get(sdi);
  const char* model = sr_dev_inst_model_get(sdi);
  device.serialNumber =
      serial != nullptr ? ownSerialNumber(serial, vendor != nullptr ? vendor : "", model != nullptr ? model : "") : "";
  if (serial != nullptr && serial[0] != '\0' && device.serialNumber.empty()) {
    bitoscope_log(BITOSCOPE_LOG_INFO, "%s: serial number \"%s\" only repeats its name; device is known by its port",
                  device.id.c_str(), serial);
  }
  device.threshold = ThresholdSetting{};
  device.pwm = PwmGenerator{};

  if (!addToEntry(listed, device, whyNot)) {
    return BITOSCOPE_ERROR;
  }
  bitoscope_log(BITOSCOPE_LOG_INFO, "described %s \"%s\": %zu channels, %zu sample rates", device.id.c_str(),
                device.name.c_str(), device.channelNames.size(), device.sampleRatesMilliHz.size());
  return BITOSCOPE_OK;
}

}
