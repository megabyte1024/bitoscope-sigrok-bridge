/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_PWM_GENERATOR_H
#define SIGROK_BRIDGE_PWM_GENERATOR_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "bitoscope_driver.h"

struct sr_channel_group;
struct sr_dev_driver;
struct sr_dev_inst;

namespace sigrok_bridge {

struct BridgeDevice;

inline constexpr size_t kMaxPwmOutputs = 32;

struct PwmOption {
  uint32_t key;
  bool settable;
};

struct PwmGroupOptions {
  std::string name;
  std::vector<PwmOption> options;
};

struct PwmDetection {
  std::vector<size_t> groups;
  bool truncated = false;
};

PwmDetection detectPwmOutputs(std::span<const PwmGroupOptions> groups);

std::vector<sr_channel_group*> pwmChannelGroups(const sr_dev_driver* driver, const sr_dev_inst* sdi,
                                                bool* truncated = nullptr);

struct PwmLimits {
  uint64_t minHz = 1;
  uint64_t maxHz = 0;
};

PwmLimits pwmLimitsOfRow(uint64_t rowMinHz, uint64_t rowMaxHz);

inline constexpr uint32_t kMinPwmDutyPercent = 1;
inline constexpr uint32_t kMaxPwmDutyPercent = 100;

inline constexpr uint64_t kDefaultPwmFrequencyHz = 1000;
inline constexpr uint32_t kDefaultPwmDutyPercent = 50;

struct PwmStatus {
  bool ok = true;
  std::string error;
};

class PwmOutputSetter {
 public:
  virtual ~PwmOutputSetter() = default;
  virtual PwmStatus setFrequencyHz(uint32_t output, double hertz) = 0;
  virtual PwmStatus setDutyPercent(uint32_t output, double percent) = 0;
  virtual PwmStatus setEnabled(uint32_t output, bool enabled) = 0;
};

struct PwmOutputState {
  bool enabled = false;
  uint64_t frequencyHz = kDefaultPwmFrequencyHz;
  uint32_t dutyPercent = kDefaultPwmDutyPercent;
};

struct PwmWaveform {
  std::string name;
  uint64_t squareFrequencyMilliHz = 0;
  uint32_t squareDutyPercent = 0;
};

struct PwmPropertyValue {
  std::string id;
  int64_t integer = 0;
  bool boolean = false;
};

struct PwmReply {
  bool ok = false;
  std::string message;
  std::string refusedProperty;
};

class PwmControl {
 public:
  PwmControl() = default;
  PwmControl(std::vector<std::string> groupNames, PwmLimits limits);

  size_t outputCount() const { return outputs_.size(); }
  const PwmOutputState& output(size_t index) const { return outputs_[index]; }
  const PwmLimits& limits() const { return limits_; }
  const std::string& groupName(size_t index) const { return groupNames_[index]; }

  PwmReply start(PwmOutputSetter& setter, uint32_t mask, const PwmWaveform& waveform);
  PwmReply configure(PwmOutputSetter& setter, uint32_t mask, const PwmWaveform& waveform);
  PwmReply stop(PwmOutputSetter& setter, uint32_t mask);
  PwmReply apply(PwmOutputSetter& setter, std::span<const PwmPropertyValue> values);

 private:
  enum class Step { frequency, duty, on, off };
  struct Planned {
    size_t output;
    Step step;
    uint64_t hertz;
    uint32_t percent;
  };
  struct Outcome {
    bool ok = true;
    size_t failed = 0;
    std::string error;
  };

  std::string label(size_t output) const;
  std::string outputsOfMask(uint32_t mask, std::vector<size_t>& outputs) const;
  Outcome run(PwmOutputSetter& setter, const std::vector<Planned>& plan);
  PwmReply failure(const std::vector<Planned>& plan, const Outcome& outcome) const;
  PwmReply setWaveform(PwmOutputSetter& setter, uint32_t mask, const PwmWaveform& waveform, bool turnOn);

  std::vector<std::string> groupNames_;
  PwmLimits limits_;
  std::vector<PwmOutputState> outputs_;
};

std::string pwmPropertyId(size_t output, const char* field);

std::string sigrokFailureText(int status, const std::string& lastError);

struct PwmGenerator {
  std::vector<sr_channel_group*> groups;
  PwmControl control;
};

PwmReply startPwm(BridgeDevice& device, uint32_t mask, const PwmWaveform& waveform);
PwmReply configurePwm(BridgeDevice& device, uint32_t mask, const PwmWaveform& waveform);
PwmReply stopPwm(BridgeDevice& device, uint32_t mask);
PwmReply applyPwm(BridgeDevice& device, std::span<const PwmPropertyValue> values);

bool readPwmWaveform(const bitoscope_waveform* given, PwmWaveform& waveform, bitoscope_reply* reply);

struct PwmPropertyDefinition {
  std::string id;
  std::string name;
  int type;
  std::string help;
  std::string group;
  int unit;
  bool changeableDuringCapture = false;
  bool hasMinimum = false;
  int64_t minimum = 0;
  bool hasMaximum = false;
  int64_t maximum = 0;
  int64_t defaultValue = 0;
  int booleanStyle = BITOSCOPE_BOOLEAN_ON_OFF;
};

std::vector<PwmPropertyDefinition> pwmPropertyDefinitions(std::span<const std::string> groupNames,
                                                          const PwmLimits& limits);

void addPwmGenerator(bitoscope_device* listed, BridgeDevice& device);

}

#endif
