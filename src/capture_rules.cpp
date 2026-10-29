/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "capture_rules.h"

#include <algorithm>
#include <limits>

namespace sigrok_bridge {

namespace {

std::string plural(uint64_t count, const char* one, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? one : many);
}

std::string seconds(int64_t milliseconds) { return std::to_string(milliseconds / 1000) + " s"; }

constexpr uint64_t kLargestUnitSizeAccepted = 0xFFFF;

constexpr int kChannelMaskBits = std::numeric_limits<uint32_t>::digits;

uint64_t positiveNumberFromText(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return 0;
  }
  uint64_t value = 0;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      return 0;
    }
    const uint64_t digit = static_cast<uint64_t>(*c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return 0;
    }
    value = value * 10 + digit;
  }
  return value;
}

}

std::string startRefusal(const StartRequest& request) {
  if (request.buffered) {
    return "the sigrok bridge only streams: buffered captures are not supported";
  }
  if (!request.deviceConnected) {
    return "the device is no longer connected, or another program is using it";
  }
  if (request.sessionAbandoned) {
    return "libsigrok did not stop this device after an earlier capture: the driver has to be restarted";
  }
  if (request.channelMask == 0) {
    return "no channel is selected";
  }
  if (request.sampleRateMilliHz == 0 || request.sampleRateMilliHz % kMilliHertzPerHertz != 0) {
    return "a sample rate of " + std::to_string(request.sampleRateMilliHz) +
           " mHz is not a whole number of hertz, which is what libsigrok takes";
  }
  if (request.row != nullptr && request.row->startRefusal != nullptr) {
    const char* why = request.row->startRefusal(request.channelMask, request.sampleRateMilliHz / kMilliHertzPerHertz);
    if (why != nullptr) {
      return why;
    }
  }
  return {};
}

bool channelEnabledInCapture(bool isLogicChannel, int index, uint32_t channelMask) {
  return isLogicChannel && index >= 0 && index < kChannelMaskBits && (channelMask >> index & 1) != 0;
}

uint64_t testLimitSamplesFromText(const char* text) { return positiveNumberFromText(text); }

SampleLimitPlan planSampleLimit(const SampleLimitFacts& facts) {
  SampleLimitPlan plan;
  const bool driverTakesLimit = facts.rowEndsAtSampleLimit && facts.settable;
  const bool captureHasLimit =
      facts.captureSampleLimit != 0 && facts.captureSampleLimit < kUnreachableSampleLimit;
  plan.driverEnds = driverTakesLimit && captureHasLimit;

  if (facts.testLimit != 0) {
    plan.action = facts.settable ? SampleLimitPlan::Action::kSet : SampleLimitPlan::Action::kRefuse;
    plan.samples = facts.testLimit;
    return plan;
  }
  if (driverTakesLimit) {
    if (captureHasLimit) {
      plan.action = SampleLimitPlan::Action::kSet;
      plan.samples = facts.captureSampleLimit;
    } else if (!facts.readable || facts.inForce != 0) {
      plan.action = SampleLimitPlan::Action::kSet;
      plan.samples = kUnreachableSampleLimit;
    }
    return plan;
  }
  if (facts.readable && facts.inForce != 0) {
    plan.action = SampleLimitPlan::Action::kRefuse;
    plan.samples = facts.inForce;
  }
  return plan;
}

CaptureRules::CaptureRules(uint64_t sampleLimit, bool driverEndsAtLimit) noexcept
    : sampleLimit_(sampleLimit), driverEndsAtLimit_(driverEndsAtLimit) {}

void CaptureRules::fail(Failure failure, uint64_t first, uint64_t second) noexcept {
  if (state_ != State::kRunning) {
    return;
  }
  state_ = State::kFailed;
  failure_ = failure;
  failureNumbers_[0] = first;
  failureNumbers_[1] = second;
  failedAfterSamples_ = samplesWritten_;
}

void CaptureRules::sessionStarted(int64_t nowMs) noexcept { lastSampleMs_ = nowMs; }

uint64_t CaptureRules::logicPacket(int64_t nowMs, uint64_t lengthBytes, uint64_t unitSize) noexcept {
  if (state_ != State::kRunning) {
    return 0;
  }
  if (unitSize == 0 || unitSize > kLargestUnitSizeAccepted) {
    fail(Failure::kUnitSize, unitSize);
    return 0;
  }
  if (unitSize_ != 0 && unitSize != unitSize_) {
    fail(Failure::kUnitSizeChanged, unitSize_, unitSize);
    return 0;
  }
  unitSize_ = unitSize;
  if (lengthBytes % unitSize != 0) {
    fail(Failure::kRaggedLength, lengthBytes, unitSize);
    return 0;
  }
  const uint64_t sampleCount = lengthBytes / unitSize;
  if (sampleCount != 0) {
    lastSampleMs_ = nowMs;
  }
  return sampleCount;
}

bool CaptureRules::writeResult(int status, uint64_t sampleCount, bool hostStopping) noexcept {
  if (state_ != State::kRunning) {
    return false;
  }
  if (status == kWriteOk) {
    samplesWritten_ += sampleCount;
    return true;
  }
  if (status == kWriteOver) {
    state_ = State::kOver;
    limitReached_ = !hostStopping && sampleLimit_ != 0 && samplesWritten_ + sampleCount >= sampleLimit_;
    if (limitReached_ && driverEndsAtLimit_) {
      awaitingDriver_ = true;
      awaitingSinceMs_ = lastSampleMs_;
    }
    return false;
  }
  fail(Failure::kDelivery);
  return false;
}

void CaptureRules::deliverLogic(int64_t nowMs, const uint8_t* data, uint64_t lengthBytes, uint64_t unitSize,
                                DescribeSamples describe, WriteSamples write, void* context) noexcept {
  const uint64_t samples = logicPacket(nowMs, lengthBytes, unitSize);
  if (samples == 0) {
    return;
  }
  if (data == nullptr) {
    fail(Failure::kInternal);
    return;
  }
  if (!described_) {
    if (!describe(context, unitSize)) {
      fail(Failure::kUnitSize, unitSize);
      return;
    }
    described_ = true;
  }
  const WriteOutcome outcome = write(context, data, samples);
  (void)writeResult(outcome.status, samples, outcome.hostStopping);
}

void CaptureRules::usbFailuresAtStart(uint64_t count) noexcept { usbFailuresAtStart_ = count; }

void CaptureRules::usbFailuresNow(uint64_t count) noexcept {
  if (count > usbFailuresAtStart_) {
    fail(Failure::kUsbTransfer, count - usbFailuresAtStart_);
  }
}

void CaptureRules::endPacket(int64_t nowMs) noexcept {
  fail(Failure::kEarlyEnd);
  if (!ending_) {
    ending_ = true;
    endingSinceMs_ = nowMs;
  }
}

void CaptureRules::sessionStopped() noexcept {
  fail(Failure::kSessionEnded);
  sessionStopped_ = true;
}

void CaptureRules::internalError() noexcept { fail(Failure::kInternal); }

void CaptureRules::sinkFailed() noexcept { fail(Failure::kDelivery); }

TickAction CaptureRules::tick(int64_t nowMs, bool hostStopping) noexcept {
  if (sessionStopped_ || abandoned_) {
    return TickAction::kNothing;
  }
  if (state_ == State::kRunning) {
    if (!hostStopping) {
      return TickAction::kNothing;
    }
    state_ = State::kOver;
  }
  if (!ending_) {
    if (awaitingDriver_ && nowMs - awaitingSinceMs_ < kDriverEndLimitMs) {
      return TickAction::kNothing;
    }
    ending_ = true;
    endingSinceMs_ = nowMs;
    stopRequested_ = true;
    return TickAction::kStopSession;
  }
  if (nowMs - endingSinceMs_ >= kStopLimitMs) {
    abandoned_ = true;
    return TickAction::kAbandonSession;
  }
  return TickAction::kNothing;
}

bool CaptureRules::stopPending() const noexcept {
  return state_ != State::kRunning && !ending_ && !awaitingDriver_ && !sessionStopped_ && !abandoned_;
}

bool CaptureRules::decided() const noexcept { return state_ != State::kRunning; }

bool CaptureRules::failed() const noexcept { return state_ == State::kFailed; }

bool CaptureRules::abandoned() const noexcept { return abandoned_; }

uint64_t CaptureRules::samplesWritten() const noexcept { return samplesWritten_; }

CaptureResult CaptureRules::result(std::string_view sigrokError) const {
  CaptureResult result;
  result.sessionAbandoned = abandoned_;
  result.stopRequested = stopRequested_;
  if (state_ == State::kOver) {
    result.succeeded = true;
    result.limitReached = limitReached_;
    result.waitedForDriver = awaitingDriver_;
    return result;
  }
  const std::string after = " after " + plural(failedAfterSamples_, "sample", "samples");
  std::string what;
  switch (state_ == State::kFailed ? failure_ : Failure::kNone) {
    case Failure::kUnitSize:
      what = "libsigrok delivered samples of " + plural(failureNumbers_[0], "byte", "bytes") +
             ", which cannot hold the captured channels," + after;
      break;
    case Failure::kUnitSizeChanged:
      what = "libsigrok changed its samples from " + plural(failureNumbers_[0], "byte", "bytes") + " to " +
             plural(failureNumbers_[1], "byte", "bytes") + "," + after;
      break;
    case Failure::kRaggedLength:
      what = "libsigrok delivered a packet of " + plural(failureNumbers_[0], "byte", "bytes") +
             ", which is not a whole number of " + std::to_string(failureNumbers_[1]) + "-byte samples," +
             after;
      break;
    case Failure::kDelivery:
      what = "the samples could not be delivered to the host" + after;
      result.failureReported = true;
      break;
    case Failure::kEarlyEnd:
      what = "the device ended the capture by itself" + after + ", before a STOP or the sample limit";
      break;
    case Failure::kSessionEnded:
      what = "libsigrok's session ended by itself" + after + ", before a STOP or the sample limit";
      break;
    case Failure::kUsbTransfer:
      what = plural(failureNumbers_[0], "USB sample transfer", "USB sample transfers") + " failed" + after +
             ": the samples in " + (failureNumbers_[0] == 1 ? "it" : "them") +
             " are lost, and nothing in the stream shows where";
      break;
    case Failure::kInternal:
      what = "the sigrok bridge failed while handling a packet," + after;
      break;
    case Failure::kNone:
      what = "the capture ended without a reason after " + plural(samplesWritten_, "sample", "samples") +
             " (a defect of the sigrok bridge)";
      break;
  }
  result.failure = withSigrokError(what, sigrokError);
  return result;
}

std::string endingText(const CaptureResult& result) {
  std::string text = result.limitReached ? "sample limit reached" : "the host's STOP";
  if (result.waitedForDriver) {
    text += result.stopRequested ? ", but the driver did not end the session within " +
                                       seconds(kDriverEndLimitMs) + ": the bridge stopped it"
                                 : ", and the driver ended the session itself: no stop was requested";
  } else {
    text += result.stopRequested ? ", and the bridge stopped the session"
                                 : ", and the device ended the session before the bridge asked";
  }
  if (result.sessionAbandoned) {
    text += " - the session never ended";
  }
  return text;
}

std::string withSigrokError(std::string_view what, std::string_view sigrokError) {
  std::string text(what);
  if (!sigrokError.empty()) {
    text += " (libsigrok: ";
    text += sigrokError;
    text += ")";
  }
  return text;
}

CaptureResult startFailure(StartStage stage, std::string_view what, std::string_view sigrokError) {
  CaptureResult result;
  result.failure = withSigrokError(what, sigrokError);
  result.failedAt = stage;
  result.deviceOpened = stage == StartStage::kSetUp || stage == StartStage::kAcquisitionStart;
  return result;
}

bool deviceProbedAfter(const CaptureResult& result) {
  return !result.succeeded && !result.sessionAbandoned && result.deviceOpened;
}

DeviceState deviceStateAfterFailure(const DeviceProbe& probe) {
  if (probe.reopened) {
    return DeviceState::kOpen;
  }
  if (probe.hasBusId && probe.busKnown && !probe.onBus) {
    return DeviceState::kGone;
  }
  return DeviceState::kClosed;
}

std::vector<EndStep> endSteps(const CaptureResult& result, std::string_view deviceName, DeviceState state,
                              std::string_view reopenError, bool lostAlreadyReported) {
  std::vector<EndStep> steps;
  const std::string name(deviceName);
  const bool gone = state == DeviceState::kGone && !result.sessionAbandoned;
  const bool notReopened = state == DeviceState::kClosed && !result.sessionAbandoned;
  const bool lost = result.sessionAbandoned || gone;
  std::string lostReason;
  if (result.sessionAbandoned) {
    lostReason = name + ": libsigrok did not stop the device within " + seconds(kStopLimitMs) +
                 " of being asked to; the driver has to be restarted";
  } else if (gone) {
    lostReason = name + " is gone: it is no longer on the USB bus after its capture failed";
  }
  const std::string reopenText =
      withSigrokError("the device could not be re-opened after the failed capture", reopenError);

  if (lost && !lostAlreadyReported) {
    steps.push_back({EndStep::Kind::kDeviceLost, lostReason});
  }
  if (!result.succeeded && !result.failureReported) {
    std::string failure = result.failure;
    if (gone) {
      failure += "; the device is gone (it is no longer on the USB bus)";
    } else if (notReopened) {
      failure += "; " + reopenText;
    }
    steps.push_back({EndStep::Kind::kFailCapture, std::move(failure)});
  } else if (notReopened) {
    steps.push_back({EndStep::Kind::kLogError, name + ": " + reopenText});
  }
  if (lost) {
    std::string line = lostReason;
    if (!result.succeeded) {
      line += " - the capture failed: " + result.failure;
    }
    steps.push_back({EndStep::Kind::kLogError, std::move(line)});
  }
  return steps;
}

}
