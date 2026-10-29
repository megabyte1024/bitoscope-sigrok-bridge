/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_CAPTURE_RULES_H
#define SIGROK_BRIDGE_CAPTURE_RULES_H

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "usb_id_table.h"

namespace sigrok_bridge {

struct StartRequest {
  bool buffered = false;
  bool deviceConnected = true;
  bool sessionAbandoned = false;
  uint32_t channelMask = 0;
  uint64_t sampleRateMilliHz = 0;
  const DriverRow* row = nullptr;
};

std::string startRefusal(const StartRequest& request);

bool channelEnabledInCapture(bool isLogicChannel, int index, uint32_t channelMask);

uint64_t testLimitSamplesFromText(const char* text);

enum class StartStage {
  kNone,
  kNotConnected,
  kOpen,
  kSetUp,
  kAcquisitionStart,
};

inline constexpr uint64_t kUnreachableSampleLimit = uint64_t{1} << 53;
static_assert(static_cast<uint64_t>(static_cast<double>(kUnreachableSampleLimit)) == kUnreachableSampleLimit,
              "a driver that computes with the limit as a double sees it exactly");
static_assert(kUnreachableSampleLimit * 1000 <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
              "a thousand times the limit still fits a signed 64-bit integer, whatever a driver computes with it");

inline constexpr uint64_t kMilliHertzPerHertz = 1000;

struct SampleLimitFacts {
  bool rowEndsAtSampleLimit = false;
  uint64_t captureSampleLimit = 0;
  bool settable = false;
  bool readable = false;
  uint64_t inForce = 0;
  uint64_t testLimit = 0;
};

struct SampleLimitPlan {
  enum class Action {
    kLeave,
    kSet,
    kRefuse,
  };
  Action action = Action::kLeave;
  uint64_t samples = 0;
  bool driverEnds = false;
};

SampleLimitPlan planSampleLimit(const SampleLimitFacts& facts);

inline constexpr int kWriteOk = 0;
inline constexpr int kWriteOver = 1;

inline constexpr uint32_t kSilenceLimitMs = 5000;
inline constexpr int64_t kStopLimitMs = 3000;
inline constexpr int64_t kDriverEndLimitMs = 1000;

struct WriteOutcome {
  int status = kWriteOk;
  bool hostStopping = false;
};
using DescribeSamples = bool (*)(void* context, uint64_t unitSize);
using WriteSamples = WriteOutcome (*)(void* context, const uint8_t* samples, uint64_t sampleCount);

enum class TickAction {
  kNothing,
  kStopSession,
  kAbandonSession,
};

struct CaptureResult {
  bool succeeded = false;
  std::string failure;
  bool failureReported = false;
  bool sessionAbandoned = false;
  bool deviceOpened = true;
  StartStage failedAt = StartStage::kNone;
  bool limitReached = false;
  bool waitedForDriver = false;
  bool stopRequested = false;
};

std::string endingText(const CaptureResult& result);

class CaptureRules {
 public:
  explicit CaptureRules(uint64_t sampleLimit = 0, bool driverEndsAtLimit = false) noexcept;

  void sessionStarted(int64_t nowMs) noexcept;

  uint64_t logicPacket(int64_t nowMs, uint64_t lengthBytes, uint64_t unitSize) noexcept;

  bool writeResult(int status, uint64_t sampleCount, bool hostStopping = false) noexcept;

  void deliverLogic(int64_t nowMs, const uint8_t* data, uint64_t lengthBytes, uint64_t unitSize,
                    DescribeSamples describe, WriteSamples write, void* context) noexcept;

  void usbFailuresAtStart(uint64_t count) noexcept;
  void usbFailuresNow(uint64_t count) noexcept;

  void endPacket(int64_t nowMs) noexcept;

  void sessionStopped() noexcept;

  void internalError() noexcept;

  void sinkFailed() noexcept;

  TickAction tick(int64_t nowMs, bool hostStopping) noexcept;

  bool stopPending() const noexcept;

  bool decided() const noexcept;
  bool failed() const noexcept;
  bool abandoned() const noexcept;
  uint64_t samplesWritten() const noexcept;

  CaptureResult result(std::string_view sigrokError) const;

 private:
  enum class State { kRunning, kOver, kFailed };
  enum class Failure {
    kNone,
    kUnitSize,
    kUnitSizeChanged,
    kRaggedLength,
    kDelivery,
    kEarlyEnd,
    kSessionEnded,
    kUsbTransfer,
    kInternal,
  };

  void fail(Failure failure, uint64_t first = 0, uint64_t second = 0) noexcept;

  uint64_t sampleLimit_;
  uint64_t unitSize_ = 0;
  bool described_ = false;
  bool driverEndsAtLimit_;
  State state_ = State::kRunning;
  Failure failure_ = Failure::kNone;
  uint64_t failureNumbers_[2] = {0, 0};
  uint64_t failedAfterSamples_ = 0;
  uint64_t samplesWritten_ = 0;
  uint64_t usbFailuresAtStart_ = 0;
  int64_t lastSampleMs_ = 0;
  bool ending_ = false;
  int64_t endingSinceMs_ = 0;
  bool limitReached_ = false;
  bool awaitingDriver_ = false;
  int64_t awaitingSinceMs_ = 0;
  bool stopRequested_ = false;
  bool sessionStopped_ = false;
  bool abandoned_ = false;
};

CaptureResult startFailure(StartStage stage, std::string_view what, std::string_view sigrokError = {});

std::string withSigrokError(std::string_view what, std::string_view sigrokError);

bool deviceProbedAfter(const CaptureResult& result);

struct DeviceProbe {
  bool reopened = false;
  bool hasBusId = true;
  bool busKnown = false;
  bool onBus = false;
};

enum class DeviceState {
  kOpen,
  kClosed,
  kGone,
};

DeviceState deviceStateAfterFailure(const DeviceProbe& probe);

struct EndStep {
  enum class Kind {
    kDeviceLost,
    kFailCapture,
    kLogError,
  };
  Kind kind;
  std::string text;
};

std::vector<EndStep> endSteps(const CaptureResult& result, std::string_view deviceName, DeviceState state,
                              std::string_view reopenError = {}, bool lostAlreadyReported = false);

}

#endif
