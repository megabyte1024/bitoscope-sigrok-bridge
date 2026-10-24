/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pwm_generator.h"

#include <mutex>
#include <optional>
#include <utility>

#include <libsigrok/libsigrok.h>

#include "bridge_device.h"
#include "sigrok_context.h"

namespace sigrok_bridge {

namespace {

bool hasSettableOption(const PwmGroupOptions& group, uint32_t key) {
  for (const PwmOption& option : group.options) {
    if (option.key == key && option.settable) {
      return true;
    }
  }
  return false;
}

bool isPwmOutput(const PwmGroupOptions& group) {
  return hasSettableOption(group, SR_CONF_ENABLED) && hasSettableOption(group, SR_CONF_OUTPUT_FREQUENCY) &&
         hasSettableOption(group, SR_CONF_DUTY_CYCLE);
}

std::string hertzText(uint64_t hertz) { return std::to_string(hertz) + " Hz"; }

std::string milliHertzText(uint64_t milliHertz) {
  std::string fraction = std::to_string(milliHertz % 1000);
  fraction.insert(0, 3 - fraction.size(), '0');
  return std::to_string(milliHertz / 1000) + "." + fraction + " Hz";
}

std::string settingText(uint64_t hertz, uint32_t percent) {
  return hertzText(hertz) + ", " + std::to_string(percent) + " %";
}

std::string frequencyProblem(int64_t hertz, const PwmLimits& limits) {
  if (hertz < static_cast<int64_t>(limits.minHz)) {
    return "frequency " + std::to_string(hertz) + " Hz is below the lowest the outputs make, " +
           hertzText(limits.minHz);
  }
  if (limits.maxHz != 0 && hertz > static_cast<int64_t>(limits.maxHz)) {
    return "frequency " + std::to_string(hertz) + " Hz is above the highest the outputs make, " +
           hertzText(limits.maxHz);
  }
  return {};
}

std::string dutyProblem(int64_t percent) {
  if (percent < static_cast<int64_t>(kMinPwmDutyPercent) || percent > static_cast<int64_t>(kMaxPwmDutyPercent)) {
    return "duty " + std::to_string(percent) + " % is not possible: the duty is " +
           std::to_string(kMinPwmDutyPercent) + " to " + std::to_string(kMaxPwmDutyPercent) + " %";
  }
  return {};
}

uint64_t defaultFrequencyHz(const PwmLimits& limits) {
  uint64_t hertz = kDefaultPwmFrequencyHz;
  if (hertz < limits.minHz) {
    hertz = limits.minHz;
  }
  if (limits.maxHz != 0 && hertz > limits.maxHz) {
    hertz = limits.maxHz;
  }
  return hertz;
}

PwmReply refusal(std::string message, std::string property = {}) {
  return PwmReply{false, std::move(message), std::move(property)};
}

}

PwmDetection detectPwmOutputs(std::span<const PwmGroupOptions> groups) {
  PwmDetection result;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (!isPwmOutput(groups[i])) {
      continue;
    }
    if (result.groups.size() == kMaxPwmOutputs) {
      result.truncated = true;
      break;
    }
    result.groups.push_back(i);
  }
  return result;
}

std::vector<sr_channel_group*> pwmChannelGroups(const sr_dev_driver* driver, const sr_dev_inst* sdi,
                                                bool* truncated) {
  std::vector<sr_channel_group*> all;
  std::vector<PwmGroupOptions> options;
  for (GSList* l = sr_dev_inst_channel_groups_get(sdi); l != nullptr; l = l->next) {
    auto* group = static_cast<sr_channel_group*>(l->data);
    PwmGroupOptions entry;
    entry.name = group->name != nullptr ? group->name : "";
    GArray* keys = sr_dev_options(driver, sdi, group);
    if (keys != nullptr) {
      for (guint i = 0; i < keys->len; ++i) {
        const uint32_t key = g_array_index(keys, uint32_t, i);
        bool settable = false;
        if (key == SR_CONF_ENABLED || key == SR_CONF_OUTPUT_FREQUENCY || key == SR_CONF_DUTY_CYCLE) {
          const uint32_t flags = static_cast<uint32_t>(sr_dev_config_capabilities_list(sdi, group, key));
          settable = (flags & static_cast<uint32_t>(SR_CONF_SET)) != 0;
        }
        entry.options.push_back(PwmOption{key, settable});
      }
      g_array_free(keys, TRUE);
    }
    all.push_back(group);
    options.push_back(std::move(entry));
  }
  const PwmDetection found = detectPwmOutputs(options);
  if (truncated != nullptr) {
    *truncated = found.truncated;
  }
  std::vector<sr_channel_group*> result;
  for (const size_t index : found.groups) {
    result.push_back(all[index]);
  }
  return result;
}

PwmLimits pwmLimitsOfRow(uint64_t rowMinHz, uint64_t rowMaxHz) {
  return PwmLimits{rowMinHz != 0 ? rowMinHz : 1, rowMaxHz};
}

std::string pwmPropertyId(size_t output, const char* field) {
  return "pwm" + std::to_string(output) + "_" + field;
}

std::string sigrokFailureText(int status, const std::string& lastError) {
  std::string text = sr_strerror(status);
  if (!lastError.empty()) {
    text += ": " + lastError;
  }
  return text;
}

PwmControl::PwmControl(std::vector<std::string> groupNames, PwmLimits limits)
    : groupNames_(std::move(groupNames)), limits_(limits) {
  PwmOutputState initial;
  initial.frequencyHz = defaultFrequencyHz(limits_);
  outputs_.assign(groupNames_.size(), initial);
}

std::string PwmControl::label(size_t output) const {
  return "PWM output " + std::to_string(output) + " (" + groupNames_[output] + ")";
}

std::string PwmControl::outputsOfMask(uint32_t mask, std::vector<size_t>& outputs) const {
  outputs.clear();
  if (mask == 0) {
    return "no PWM output is selected (the channel mask is empty)";
  }
  for (size_t bit = 0; bit < 32; ++bit) {
    if ((mask & (uint32_t{1} << bit)) == 0) {
      continue;
    }
    if (bit >= outputs_.size()) {
      return "PWM output " + std::to_string(bit) + " does not exist: " +
             (outputs_.empty() ? std::string("this device has no PWM outputs")
                               : "this device has " + std::to_string(outputs_.size()) + " PWM output" +
                                     (outputs_.size() == 1 ? "" : "s") + " (0 to " +
                                     std::to_string(outputs_.size() - 1) + ")");
    }
    outputs.push_back(bit);
  }
  return {};
}

PwmControl::Outcome PwmControl::run(PwmOutputSetter& setter, const std::vector<Planned>& plan) {
  Outcome outcome;
  for (size_t i = 0; i < plan.size(); ++i) {
    const Planned& item = plan[i];
    PwmStatus status;
    switch (item.step) {
      case Step::frequency:
        status = setter.setFrequencyHz(static_cast<uint32_t>(item.output), static_cast<double>(item.hertz));
        break;
      case Step::duty:
        status = setter.setDutyPercent(static_cast<uint32_t>(item.output), static_cast<double>(item.percent));
        break;
      case Step::on:
        status = setter.setEnabled(static_cast<uint32_t>(item.output), true);
        break;
      case Step::off:
        status = setter.setEnabled(static_cast<uint32_t>(item.output), false);
        break;
    }
    if (!status.ok) {
      outcome.ok = false;
      outcome.failed = i;
      outcome.error = status.error.empty() ? "libsigrok gave no reason" : status.error;
      return outcome;
    }
    PwmOutputState& state = outputs_[item.output];
    switch (item.step) {
      case Step::frequency:
        state.frequencyHz = item.hertz;
        break;
      case Step::duty:
        state.dutyPercent = item.percent;
        break;
      case Step::on:
        state.enabled = true;
        break;
      case Step::off:
        state.enabled = false;
        break;
    }
  }
  return outcome;
}

PwmReply PwmControl::failure(const std::vector<Planned>& plan, const Outcome& outcome) const {
  const Planned& item = plan[outcome.failed];
  std::string what;
  switch (item.step) {
    case Step::frequency:
      what = "setting the frequency to " + hertzText(item.hertz);
      break;
    case Step::duty:
      what = "setting the duty to " + std::to_string(item.percent) + " %";
      break;
    case Step::on:
      what = "turning the output on";
      break;
    case Step::off:
      what = "turning the output off";
      break;
  }
  std::string message = label(item.output) + ": " + what + " failed: " + outcome.error;

  std::string finished;
  size_t previous = item.output;
  for (size_t i = 0; i < outcome.failed; ++i) {
    if (plan[i].output != item.output && plan[i].output != previous) {
      finished += (finished.empty() ? "" : ", ") + label(plan[i].output);
    }
    previous = plan[i].output;
  }
  if (!finished.empty()) {
    message += "; already done before it: " + finished;
  }
  return refusal(std::move(message));
}

PwmReply PwmControl::setWaveform(PwmOutputSetter& setter, uint32_t mask, const PwmWaveform& waveform,
                                 bool turnOn) {
  const char* command = turnOn ? "start" : "configure";
  std::vector<size_t> outputs;
  const std::string maskProblem = outputsOfMask(mask, outputs);
  if (!maskProblem.empty()) {
    return refusal(maskProblem);
  }

  const bool fromCommand = waveform.name == "square";
  uint64_t hertz = 0;
  uint32_t percent = 0;
  if (fromCommand) {
    if (waveform.squareFrequencyMilliHz % 1000 != 0) {
      return refusal("frequency " + milliHertzText(waveform.squareFrequencyMilliHz) +
                     " is not a whole number of hertz: the outputs make whole hertz only");
    }
    const uint64_t wholeHertz = waveform.squareFrequencyMilliHz / 1000;
    if (std::string problem = frequencyProblem(static_cast<int64_t>(wholeHertz), limits_); !problem.empty()) {
      return refusal(std::move(problem));
    }
    if (std::string problem = dutyProblem(static_cast<int64_t>(waveform.squareDutyPercent));
        !problem.empty()) {
      return refusal(std::move(problem));
    }
    hertz = wholeHertz;
    percent = waveform.squareDutyPercent;
  } else if (waveform.name.empty()) {
    if (!turnOn) {
      return refusal("configure needs a square wave: the waveform is empty");
    }
  } else {
    return refusal(std::string(command) + " refused: waveform \"" + waveform.name +
                   "\" is not supported, the PWM outputs make a square wave only");
  }

  std::vector<Planned> plan;
  for (const size_t output : outputs) {
    const uint64_t outputHertz = fromCommand ? hertz : outputs_[output].frequencyHz;
    const uint32_t outputPercent = fromCommand ? percent : outputs_[output].dutyPercent;
    plan.push_back(Planned{output, Step::frequency, outputHertz, outputPercent});
    plan.push_back(Planned{output, Step::duty, outputHertz, outputPercent});
    if (turnOn) {
      plan.push_back(Planned{output, Step::on, outputHertz, outputPercent});
    }
  }
  const Outcome outcome = run(setter, plan);
  if (!outcome.ok) {
    return failure(plan, outcome);
  }
  std::string message = turnOn ? "started " : "configured (not turned on) ";
  for (size_t i = 0; i < outputs.size(); ++i) {
    const PwmOutputState& state = outputs_[outputs[i]];
    message += (i == 0 ? "" : "; ") + label(outputs[i]) + " at " + settingText(state.frequencyHz, state.dutyPercent);
  }
  return PwmReply{true, std::move(message), {}};
}

PwmReply PwmControl::start(PwmOutputSetter& setter, uint32_t mask, const PwmWaveform& waveform) {
  return setWaveform(setter, mask, waveform, true);
}

PwmReply PwmControl::configure(PwmOutputSetter& setter, uint32_t mask, const PwmWaveform& waveform) {
  return setWaveform(setter, mask, waveform, false);
}

PwmReply PwmControl::stop(PwmOutputSetter& setter, uint32_t mask) {
  std::vector<size_t> outputs;
  const std::string maskProblem = outputsOfMask(mask, outputs);
  if (!maskProblem.empty()) {
    return refusal(maskProblem);
  }
  std::vector<Planned> plan;
  for (const size_t output : outputs) {
    plan.push_back(Planned{output, Step::off, outputs_[output].frequencyHz, outputs_[output].dutyPercent});
  }
  const Outcome outcome = run(setter, plan);
  if (!outcome.ok) {
    return failure(plan, outcome);
  }
  std::string message = "stopped ";
  for (size_t i = 0; i < outputs.size(); ++i) {
    message += (i == 0 ? "" : "; ") + label(outputs[i]);
  }
  return PwmReply{true, std::move(message), {}};
}

PwmReply PwmControl::apply(PwmOutputSetter& setter, std::span<const PwmPropertyValue> values) {
  struct Wanted {
    std::optional<bool> enabled;
    std::optional<uint64_t> hertz;
    std::optional<uint32_t> percent;
    std::string enabledId, frequencyId, dutyId;
  };
  std::vector<Wanted> wanted(outputs_.size());

  for (const PwmPropertyValue& value : values) {
    bool known = false;
    for (size_t output = 0; output < outputs_.size() && !known; ++output) {
      Wanted& want = wanted[output];
      if (value.id == pwmPropertyId(output, "enabled")) {
        known = true;
        want.enabled = value.boolean;
        want.enabledId = value.id;
      } else if (value.id == pwmPropertyId(output, "frequency")) {
        known = true;
        if (std::string problem = frequencyProblem(value.integer, limits_); !problem.empty()) {
          return refusal(label(output) + ": " + problem, value.id);
        }
        want.hertz = static_cast<uint64_t>(value.integer);
        want.frequencyId = value.id;
      } else if (value.id == pwmPropertyId(output, "duty")) {
        known = true;
        if (std::string problem = dutyProblem(value.integer); !problem.empty()) {
          return refusal(label(output) + ": " + problem, value.id);
        }
        want.percent = static_cast<uint32_t>(value.integer);
        want.dutyId = value.id;
      }
    }
    if (!known) {
      return refusal("this device's PWM generator has no property \"" + value.id + "\"", value.id);
    }
  }

  std::vector<Planned> plan;
  std::vector<size_t> touched;
  for (size_t output = 0; output < wanted.size(); ++output) {
    const Wanted& want = wanted[output];
    if (!want.enabled && !want.hertz && !want.percent) {
      continue;
    }
    touched.push_back(output);
    const uint64_t hertz = want.hertz ? *want.hertz : outputs_[output].frequencyHz;
    const uint32_t percent = want.percent ? *want.percent : outputs_[output].dutyPercent;
    if (want.enabled && *want.enabled) {
      plan.push_back(Planned{output, Step::frequency, hertz, percent});
      plan.push_back(Planned{output, Step::duty, hertz, percent});
      plan.push_back(Planned{output, Step::on, hertz, percent});
      continue;
    }
    if (want.enabled) {
      plan.push_back(Planned{output, Step::off, hertz, percent});
    }
    if (want.hertz) {
      plan.push_back(Planned{output, Step::frequency, hertz, percent});
    }
    if (want.percent) {
      plan.push_back(Planned{output, Step::duty, hertz, percent});
    }
  }
  if (plan.empty()) {
    return PwmReply{true, "no PWM property to set", {}};
  }

  const Outcome outcome = run(setter, plan);
  if (!outcome.ok) {
    PwmReply reply = failure(plan, outcome);
    const Planned& item = plan[outcome.failed];
    const Wanted& want = wanted[item.output];
    const std::string& own = item.step == Step::frequency ? want.frequencyId
                             : item.step == Step::duty    ? want.dutyId
                                                          : want.enabledId;
    reply.refusedProperty = !own.empty() ? own : want.enabledId;
    return reply;
  }
  std::string message = "set ";
  for (size_t i = 0; i < touched.size(); ++i) {
    const size_t output = touched[i];
    const Wanted& want = wanted[output];
    const PwmOutputState& state = outputs_[output];
    message += (i == 0 ? "" : "; ") + label(output) + ": ";
    if (want.enabled && *want.enabled) {
      message += "on at " + settingText(state.frequencyHz, state.dutyPercent);
    } else if (want.enabled) {
      message += "off";
    } else {
      message += settingText(state.frequencyHz, state.dutyPercent) + (state.enabled ? " (on)" : " (off)");
    }
  }
  return PwmReply{true, std::move(message), {}};
}

namespace {

class SigrokPwmOutputs final : public PwmOutputSetter {
 public:
  explicit SigrokPwmOutputs(BridgeDevice& device) : device_(device) {}

  PwmStatus setFrequencyHz(uint32_t output, double hertz) override {
    if (!known(output)) {
      return unknown(output);
    }
    return set(output, SR_CONF_OUTPUT_FREQUENCY, g_variant_new_double(hertz));
  }
  PwmStatus setDutyPercent(uint32_t output, double percent) override {
    if (!known(output)) {
      return unknown(output);
    }
    return set(output, SR_CONF_DUTY_CYCLE, g_variant_new_double(percent));
  }
  PwmStatus setEnabled(uint32_t output, bool enabled) override {
    if (!known(output)) {
      return unknown(output);
    }
    return set(output, SR_CONF_ENABLED, g_variant_new_boolean(enabled ? TRUE : FALSE));
  }

 private:
  bool known(uint32_t output) const { return output < device_.pwm.groups.size(); }
  static PwmStatus unknown(uint32_t output) {
    return PwmStatus{false, "the device has no channel group for output " + std::to_string(output)};
  }
  PwmStatus set(uint32_t output, uint32_t key, GVariant* value) {
    clearLastSigrokError();
    const int status = sr_config_set(device_.sdi, device_.pwm.groups[output], key, value);
    if (status == SR_OK) {
      return PwmStatus{};
    }
    return PwmStatus{false, sigrokFailureText(status, lastSigrokError())};
  }

  BridgeDevice& device_;
};

template <typename Command>
PwmReply runOnDevice(BridgeDevice& device, Command&& command) {
  std::lock_guard lock(sigrokMutex());
  if (device.capturing.load()) {
    return refusal("not while a capture is running: " + device.name + " is capturing, its PWM outputs cannot be changed");
  }
  if (device.sdi == nullptr) {
    return refusal("device is no longer connected: " + device.name + " is gone");
  }
  clearLastSigrokError();
  const int status = openDevice(device);
  if (status != SR_OK) {
    return refusal("cannot open " + device.name + ": " + sigrokFailureText(status, lastSigrokError()));
  }
  SigrokPwmOutputs setter(device);
  return command(device.pwm.control, setter);
}

}

PwmReply startPwm(BridgeDevice& device, uint32_t mask, const PwmWaveform& waveform) {
  return runOnDevice(device, [&](PwmControl& control, PwmOutputSetter& setter) {
    return control.start(setter, mask, waveform);
  });
}

PwmReply configurePwm(BridgeDevice& device, uint32_t mask, const PwmWaveform& waveform) {
  return runOnDevice(device, [&](PwmControl& control, PwmOutputSetter& setter) {
    return control.configure(setter, mask, waveform);
  });
}

PwmReply stopPwm(BridgeDevice& device, uint32_t mask) {
  return runOnDevice(device, [&](PwmControl& control, PwmOutputSetter& setter) {
    return control.stop(setter, mask);
  });
}

PwmReply applyPwm(BridgeDevice& device, std::span<const PwmPropertyValue> values) {
  return runOnDevice(device, [&](PwmControl& control, PwmOutputSetter& setter) {
    return control.apply(setter, values);
  });
}

namespace {

BridgeDevice& deviceOf(void* session) { return *static_cast<BridgeDevice*>(session); }

int finish(bitoscope_reply* reply, const PwmReply& result) {
  bitoscope_reply_message(reply, result.message.c_str());
  if (!result.ok && !result.refusedProperty.empty()) {
    bitoscope_reply_refused_property(reply, result.refusedProperty.c_str());
  }
  return result.ok ? BITOSCOPE_OK : BITOSCOPE_ERROR;
}

int startOutputs(void* session, uint32_t channelMask, const bitoscope_waveform* waveform, bitoscope_reply* reply) {
  PwmWaveform wave;
  if (!readPwmWaveform(waveform, wave, reply)) {
    return BITOSCOPE_ERROR;
  }
  return finish(reply, startPwm(deviceOf(session), channelMask, wave));
}

int configureOutputs(void* session, uint32_t channelMask, const bitoscope_waveform* waveform,
                     bitoscope_reply* reply) {
  PwmWaveform wave;
  if (!readPwmWaveform(waveform, wave, reply)) {
    return BITOSCOPE_ERROR;
  }
  return finish(reply, configurePwm(deviceOf(session), channelMask, wave));
}

int stopOutputs(void* session, uint32_t channelMask, bitoscope_reply* reply) {
  return finish(reply, stopPwm(deviceOf(session), channelMask));
}

int applyProperties(void* session, const bitoscope_property_value* values, size_t numValues, int capturing,
                    bitoscope_reply* reply) {
  (void)capturing;
  std::vector<PwmPropertyValue> given;
  for (size_t i = 0; i < numValues; ++i) {
    given.push_back(PwmPropertyValue{values[i].id != nullptr ? values[i].id : "", values[i].integer,
                                     values[i].boolean != 0});
  }
  return finish(reply, applyPwm(deviceOf(session), given));
}

}

bool readPwmWaveform(const bitoscope_waveform* given, PwmWaveform& waveform, bitoscope_reply* reply) {
  waveform = PwmWaveform{};
  if (given == nullptr || given->name == nullptr) {
    return true;
  }
  waveform.name = given->name;
  if (waveform.name != "square") {
    return true;
  }
  bitoscope_reply_message(reply, "the square wave's parameters cannot be read");
  bitoscope_square_wave square{};
  if (bitoscope_read_square_wave(given, &square, reply) != BITOSCOPE_OK) {
    return false;
  }
  bitoscope_reply_message(reply, "");
  waveform.squareFrequencyMilliHz = square.frequency_milli_hz;
  waveform.squareDutyPercent = square.duty_percent;
  return true;
}

std::vector<PwmPropertyDefinition> pwmPropertyDefinitions(std::span<const std::string> groupNames,
                                                          const PwmLimits& limits) {
  std::vector<PwmPropertyDefinition> list;
  for (size_t output = 0; output < groupNames.size(); ++output) {
    const std::string group = "PWM output " + std::to_string(output) + " (" + groupNames[output] + ")";

    PwmPropertyDefinition enabled;
    enabled.id = pwmPropertyId(output, "enabled");
    enabled.name = "Output";
    enabled.type = BITOSCOPE_PROPERTY_BOOLEAN;
    enabled.help = "Turns the output's square wave on or off.";
    enabled.group = group;
    enabled.unit = BITOSCOPE_UNIT_NONE;
    enabled.booleanStyle = BITOSCOPE_BOOLEAN_ON_OFF;
    enabled.defaultValue = 0;
    list.push_back(std::move(enabled));

    PwmPropertyDefinition frequency;
    frequency.id = pwmPropertyId(output, "frequency");
    frequency.name = "Frequency";
    frequency.type = BITOSCOPE_PROPERTY_INTEGER;
    frequency.help = "The square wave's frequency, in whole hertz.";
    frequency.group = group;
    frequency.unit = BITOSCOPE_UNIT_HERTZ;
    frequency.hasMinimum = true;
    frequency.minimum = static_cast<int64_t>(limits.minHz);
    frequency.hasMaximum = limits.maxHz != 0;
    frequency.maximum = static_cast<int64_t>(limits.maxHz);
    frequency.defaultValue = static_cast<int64_t>(defaultFrequencyHz(limits));
    list.push_back(std::move(frequency));

    PwmPropertyDefinition duty;
    duty.id = pwmPropertyId(output, "duty");
    duty.name = "Duty";
    duty.type = BITOSCOPE_PROPERTY_INTEGER;
    duty.help = "The share of each period the output is high, in whole percent.";
    duty.group = group;
    duty.unit = BITOSCOPE_UNIT_PERCENT;
    duty.hasMinimum = true;
    duty.minimum = kMinPwmDutyPercent;
    duty.hasMaximum = true;
    duty.maximum = kMaxPwmDutyPercent;
    duty.defaultValue = kDefaultPwmDutyPercent;
    list.push_back(std::move(duty));
  }
  return list;
}

void addPwmGenerator(bitoscope_device* listed, BridgeDevice& device) {
  device.pwm = PwmGenerator{};
  if (device.driver == nullptr || device.sdi == nullptr || device.row == nullptr) {
    return;
  }
  bool truncated = false;
  std::vector<sr_channel_group*> groups = pwmChannelGroups(device.driver, device.sdi, &truncated);
  if (groups.empty()) {
    return;
  }
  if (truncated) {
    bitoscope_log(BITOSCOPE_LOG_WARNING, "%s has more than %zu PWM outputs: only the first %zu are listed",
                  device.id.c_str(), kMaxPwmOutputs, kMaxPwmOutputs);
  }
  std::vector<std::string> names;
  for (const sr_channel_group* group : groups) {
    names.push_back(group->name != nullptr ? group->name : "");
  }
  const PwmLimits limits = pwmLimitsOfRow(device.row->pwmMinFrequencyHz, device.row->pwmMaxFrequencyHz);

  static const char* const kWaveforms[] = {"square"};
  bitoscope_signal_generator generator{};
  generator.num_channels = static_cast<uint32_t>(groups.size());
  generator.start = startOutputs;
  generator.stop = stopOutputs;
  generator.configure = configureOutputs;
  generator.waveforms = kWaveforms;
  generator.num_waveforms = 1;
  generator.min_frequency_milli_hz = limits.minHz * 1000;
  generator.max_frequency_milli_hz = limits.maxHz * 1000;
  generator.supports_synchronized_start = 0;
  generator.apply_properties = applyProperties;
  if (listed != nullptr && bitoscope_add_signal_generator(listed, &generator) != BITOSCOPE_OK) {
    bitoscope_log(BITOSCOPE_LOG_ERROR, "the SDK refused the signal generator of %s", device.id.c_str());
    return;
  }
  device.pwm.groups = std::move(groups);
  device.pwm.control = PwmControl(names, limits);

  for (const PwmPropertyDefinition& definition : pwmPropertyDefinitions(names, limits)) {
    if (listed == nullptr) {
      break;
    }
    bitoscope_property property{};
    property.id = definition.id.c_str();
    property.name = definition.name.c_str();
    property.type = definition.type;
    property.help = definition.help.c_str();
    property.group = definition.group.c_str();
    property.unit = definition.unit;
    property.changeable_during_capture = definition.changeableDuringCapture ? 1 : 0;
    property.has_minimum = definition.hasMinimum ? 1 : 0;
    property.minimum = definition.minimum;
    property.has_maximum = definition.hasMaximum ? 1 : 0;
    property.maximum = definition.maximum;
    property.default_value = definition.defaultValue;
    property.boolean_style = definition.booleanStyle;
    if (bitoscope_add_property(listed, BITOSCOPE_SIGNAL_GENERATOR_FACET, &property) != BITOSCOPE_OK) {
      bitoscope_log(BITOSCOPE_LOG_ERROR, "the SDK refused the property %s of %s", definition.id.c_str(),
                    device.id.c_str());
    }
  }
  bitoscope_log(BITOSCOPE_LOG_INFO, "%s: %zu PWM outputs (%s ...), %llu to %llu Hz", device.id.c_str(),
                device.pwm.groups.size(), names.front().c_str(), static_cast<unsigned long long>(limits.minHz),
                static_cast<unsigned long long>(limits.maxHz));
}

}
