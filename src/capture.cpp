/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "capture.h"

#ifdef _WIN32
#include <windows.h>

#include <timeapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <bridge_usb/bridge_usb.h>

#include "bridge_device.h"
#include "capture_rules.h"
#include "core_keeper.h"
#include "device_scan.h"
#include "sigrok_context.h"
#include "threshold.h"

namespace sigrok_bridge {

namespace {

constexpr int64_t kSdkStopLimitMarginMs = 500;
static_assert(kDriverEndLimitMs + kStopLimitMs + kSdkStopLimitMarginMs <= BITOSCOPE_STOP_LIMIT_MS,
              "a capture's worst end stays half a second inside the SDK's limit from STOP to the capture's return");

constexpr guint kTickMs = 50;

constexpr gint kWakePriority = G_PRIORITY_DEFAULT;

int64_t nowMs() {
  const auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count();
}

constexpr uint32_t kCanGet = static_cast<uint32_t>(SR_CONF_GET);
constexpr uint32_t kCanSet = static_cast<uint32_t>(SR_CONF_SET);

bool can(const BridgeDevice& device, uint32_t key, uint32_t capability) {
  const int capabilities = sr_dev_config_capabilities_list(device.sdi, nullptr, static_cast<int>(key));
  return (static_cast<uint32_t>(capabilities) & capability) != 0;
}

bool readUint64(const BridgeDevice& device, uint32_t key, uint64_t& value) {
  if (!can(device, key, kCanGet)) {
    return false;
  }
  GVariant* variant = nullptr;
  if (sr_config_get(device.driver, device.sdi, nullptr, key, &variant) != SR_OK || variant == nullptr) {
    return false;
  }
  const bool isNumber = g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT64) != FALSE;
  if (isNumber) {
    value = g_variant_get_uint64(variant);
  }
  g_variant_unref(variant);
  return isNumber;
}

std::string limitNotCleared(const char* name, uint64_t limit) {
  return std::string("libsigrok holds a ") + name + " of " + std::to_string(limit) +
         " on the device, which the bridge cannot clear (sr_config_set() does not take 0)";
}

std::string timeLimitInForce(const BridgeDevice& device) {
  uint64_t limit = 0;
  if (readUint64(device, SR_CONF_LIMIT_MSEC, limit) && limit != 0) {
    return limitNotCleared("time limit (ms)", limit);
  }
  return {};
}

std::string hexMask(uint32_t mask) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%X", mask);
  return text;
}

std::string configureDevice(BridgeDevice& device, const bitoscope_capture& capture, bool& driverEnds) {
  sr_dev_inst* sdi = device.sdi;
  driverEnds = false;

  uint32_t enabledMask = 0;
  for (GSList* l = sr_dev_inst_channels_get(sdi); l != nullptr; l = l->next) {
    auto* channel = static_cast<sr_channel*>(l->data);
    const bool wanted =
        channelEnabledInCapture(channel->type == SR_CHANNEL_LOGIC, channel->index, capture.channel_mask);
    if (sr_dev_channel_enable(channel, wanted ? TRUE : FALSE) != SR_OK) {
      return std::string("cannot ") + (wanted ? "enable" : "disable") + " the device's channel " +
             (channel->name != nullptr ? channel->name : "?");
    }
    if (wanted) {
      enabledMask |= uint32_t{1} << channel->index;
    }
  }
  if (enabledMask != capture.channel_mask) {
    return "the device has the channels " + hexMask(enabledMask) + " of the capture's " +
           hexMask(capture.channel_mask) + " only";
  }

  const uint64_t rateHz = capture.sample_rate_milli_hz / kMilliHertzPerHertz;
  if (sr_config_set(sdi, nullptr, SR_CONF_SAMPLERATE, g_variant_new_uint64(rateHz)) != SR_OK) {
    return "cannot set the sample rate of " + std::to_string(rateHz) + " Hz";
  }
  uint64_t actualRateHz = 0;
  if (readUint64(device, SR_CONF_SAMPLERATE, actualRateHz) && actualRateHz != rateHz) {
    return "the device took a sample rate of " + std::to_string(actualRateHz) + " Hz in place of " +
           std::to_string(rateHz) + " Hz";
  }

  if (applyThreshold(device, capture) != SR_OK) {
    return "cannot set the threshold";
  }

  if (can(device, SR_CONF_CONTINUOUS, kCanSet) &&
      sr_config_set(sdi, nullptr, SR_CONF_CONTINUOUS, g_variant_new_boolean(TRUE)) != SR_OK) {
    return "cannot set the device to stream (SR_CONF_CONTINUOUS)";
  }

  if (std::string failed = applySampleLimit(device, capture.sample_limit, driverEnds); !failed.empty()) {
    return failed;
  }
  if (can(device, SR_CONF_LIMIT_FRAMES, kCanSet) &&
      sr_config_set(sdi, nullptr, SR_CONF_LIMIT_FRAMES, g_variant_new_uint64(0)) != SR_OK) {
    return "cannot clear the device's frame limit";
  }

  if (std::string_view(device.driver->name) == "demo") {
    const sr_channel_group* logicGroup = nullptr;
    for (GSList* l = sr_dev_inst_channel_groups_get(sdi); l != nullptr; l = l->next) {
      const auto* group = static_cast<const sr_channel_group*>(l->data);
      if (group->name != nullptr && std::string_view(group->name) == "Logic") {
        logicGroup = group;
      }
    }
    if (logicGroup == nullptr ||
        sr_config_set(sdi, logicGroup, SR_CONF_PATTERN_MODE, g_variant_new_string("incremental")) != SR_OK) {
      return "cannot set the demo device's pattern to incremental";
    }
  }
  return {};
}

struct Session {
  Session(BridgeDevice& capturedDevice, bitoscope_sink* captureSink, uint64_t sampleLimit,
          bool driverEnds)
      : device(capturedDevice),
        sink(captureSink),
        rules(sampleLimit, driverEnds) {
    std::string neverStop;
    testNeverStop = readEnvironment("BITOSCOPE_SIGROK_BRIDGE_TEST_NEVER_STOP", neverStop) && neverStop == "1";
    if (testNeverStop) {
      bitoscope_log(BITOSCOPE_LOG_WARNING,
                    "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_NEVER_STOP is set: the bridge will not stop this "
                    "capture's libsigrok session, as if the driver ignored the stop");
    }
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() { close(); }

  std::string start(bool& acquisitionStartFailed);
  void run();
  void close() noexcept;

  BridgeDevice& device;
  bitoscope_sink* sink;
  CaptureRules rules;
  bool testNeverStop = false;

  sr_session* session = nullptr;
  GMainContext* context = nullptr;
  GMainLoop* loop = nullptr;
  GSource* timer = nullptr;
  GSource* wake = nullptr;

  std::string sigrokError;
  bool sigrokErrorKept = false;

  int64_t startedMs = 0;
  int64_t decidedMs = 0;
};

void keepSigrokError(Session& session) noexcept {
  if (session.sigrokErrorKept || !session.rules.failed()) {
    return;
  }
  session.sigrokErrorKept = true;
  try {
    session.sigrokError = lastSigrokError();
  } catch (...) {
  }
}

void noteDecided(Session& session, int64_t now) noexcept {
  if (session.decidedMs == 0 && session.rules.decided()) {
    session.decidedMs = now;
  }
}

void tick(Session& session) noexcept {
  const int64_t now = nowMs();
  session.rules.usbFailuresNow(bridge_usb_failed_transfer_count());
  if (bitoscope_capture_failed(session.sink) != 0) {
    session.rules.sinkFailed();
  }
  const TickAction action = session.rules.tick(now, bitoscope_stopping(session.sink) != 0);
  noteDecided(session, now);
  switch (action) {
    case TickAction::kStopSession:
      if (!session.testNeverStop) {
        sr_session_stop(session.session);
      } else {
        bitoscope_log(BITOSCOPE_LOG_WARNING,
                      "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_NEVER_STOP: the session is not stopped now");
      }
      break;
    case TickAction::kAbandonSession:
      g_main_loop_quit(session.loop);
      break;
    case TickAction::kNothing:
      break;
  }
  keepSigrokError(session);
}

gboolean onTimer(gpointer data) {
  tick(*static_cast<Session*>(data));
  return G_SOURCE_CONTINUE;
}

gboolean onWake(gpointer data) {
  tick(*static_cast<Session*>(data));
  return G_SOURCE_REMOVE;
}

void afterEvent(Session& session) noexcept {
  keepSigrokError(session);
  noteDecided(session, nowMs());
  if (session.wake == nullptr && session.rules.stopPending()) {
    session.wake = g_idle_source_new();
    g_source_set_priority(session.wake, kWakePriority);
    g_source_set_callback(session.wake, onWake, &session, nullptr);
    g_source_attach(session.wake, session.context);
  }
}

struct SinkTarget {
  bitoscope_sink* sink;
  uint32_t deviceChannels;
};

bool describeSamples(void* context, uint64_t unitSize) {
  const auto& target = *static_cast<const SinkTarget*>(context);
  const uint64_t carriedBits = unitSize * 8;
  bitoscope_device_bytes bytes{};
  bytes.layout = BITOSCOPE_SAMPLE_MAJOR;
  bytes.channel_mask = carriedBits >= target.deviceChannels ? 0 : (uint32_t{1} << carriedBits) - 1;
  bytes.stride_bytes = static_cast<uint32_t>(unitSize);
  return bitoscope_set_device_bytes(target.sink, &bytes) == BITOSCOPE_OK;
}

WriteOutcome writeNow(void* context, const uint8_t* samples, uint64_t sampleCount) {
  auto* sink = static_cast<const SinkTarget*>(context)->sink;
  WriteOutcome outcome;
  if (bitoscope_write_samples_now(sink, samples, sampleCount) == BITOSCOPE_OK) {
    outcome.status = kWriteOk;
  } else if (bitoscope_capture_failed(sink) != 0) {
    outcome.status = BITOSCOPE_ERROR;
  } else {
    outcome.status = kWriteOver;
    outcome.hostStopping = bitoscope_stopping(sink) != 0;
  }
  return outcome;
}

void onDatafeed(const sr_dev_inst* sdi, const sr_datafeed_packet* packet, void* data) {
  (void)sdi;
  Session& session = *static_cast<Session*>(data);
  session.rules.usbFailuresNow(bridge_usb_failed_transfer_count());
  feedPacket(session.rules, session.sink, static_cast<uint32_t>(session.device.channelNames.size()), packet, nowMs());
  afterEvent(session);
}

void onStopped(void* data) {
  Session& session = *static_cast<Session*>(data);
  session.rules.sessionStopped();
  keepSigrokError(session);
  g_main_loop_quit(session.loop);
}

std::string Session::start(bool& acquisitionStartFailed) {
  acquisitionStartFailed = false;
  context = g_main_context_new();
  g_main_context_push_thread_default(context);
  loop = g_main_loop_new(context, FALSE);

  sr_context* sigrok = sigrokContext();
  if (sigrok == nullptr) {
    return "libsigrok did not start";
  }
  if (sr_session_new(sigrok, &session) != SR_OK) {
    session = nullptr;
    return "cannot make a libsigrok session";
  }
  if (sr_session_dev_add(session, device.sdi) != SR_OK) {
    return "cannot add the device to the libsigrok session";
  }
  if (sr_session_datafeed_callback_add(session, onDatafeed, this) != SR_OK ||
      sr_session_stopped_callback_set(session, onStopped, this) != SR_OK) {
    return "cannot listen to the libsigrok session";
  }
  rules.usbFailuresAtStart(bridge_usb_failed_transfer_count());
  if (sr_session_start(session) != SR_OK) {
    acquisitionStartFailed = true;
    return "libsigrok could not start the acquisition";
  }
  startedMs = nowMs();
  rules.sessionStarted(startedMs);

  timer = g_timeout_source_new(kTickMs);
  g_source_set_callback(timer, onTimer, this, nullptr);
  g_source_attach(timer, context);
  return {};
}

void Session::run() {
#ifdef _WIN32
  constexpr UINT kSystemTimerPeriodMs = 1;
  const bool fineTimer = timeBeginPeriod(kSystemTimerPeriodMs) == TIMERR_NOERROR;
#endif
  g_main_loop_run(loop);
#ifdef _WIN32
  if (fineTimer) {
    timeEndPeriod(kSystemTimerPeriodMs);
  }
#endif
}

void Session::close() noexcept {
  for (GSource** source : {&timer, &wake}) {
    if (*source != nullptr) {
      g_source_destroy(*source);
      g_source_unref(*source);
      *source = nullptr;
    }
  }
  const bool abandoned = rules.abandoned();
  if (loop != nullptr) {
    g_main_loop_unref(loop);
    loop = nullptr;
  }
  if (context != nullptr) {
    g_main_context_pop_thread_default(context);
    if (!abandoned) {
      std::lock_guard lock(sigrokMutex());
      g_main_context_unref(context);
    }
    context = nullptr;
  }
  if (session != nullptr && !abandoned) {
    std::lock_guard lock(sigrokMutex());
    sr_session_destroy(session);
  }
  session = nullptr;
}

bridge_usb_counters usbCounters() {
  bridge_usb_counters counters{};
  bridge_usb_read_counters(&counters);
  return counters;
}

bool hasSampleEndpoint(const BridgeDevice& device) {
  return device.row != nullptr && device.row->sampleEndpoint != 0;
}

void logRawIo(const BridgeDevice& device, const bridge_usb_counters& before) {
#ifdef _WIN32
  if (!hasSampleEndpoint(device)) {
    return;
  }
  const unsigned endpoint = device.row->sampleEndpoint;
  const bridge_usb_counters after = usbCounters();
  if (after.raw_io_enabled != before.raw_io_enabled) {
    bitoscope_log(BITOSCOPE_LOG_INFO, "%s: WinUSB RAW_IO is on for the sample endpoint 0x%02x",
                  device.id.c_str(), endpoint);
  }
  if (after.raw_io_failed != before.raw_io_failed) {
    bitoscope_log(BITOSCOPE_LOG_WARNING,
                  "%s: WinUSB RAW_IO could not be switched on for the sample endpoint 0x%02x: the device may "
                  "overflow, and its captures then fail",
                  device.id.c_str(), endpoint);
  }
  if (usbRawIoOffForTest()) {
    bitoscope_log(BITOSCOPE_LOG_WARNING,
                  "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_NO_RAW_IO is set: %s captures WITHOUT WinUSB RAW_IO "
                  "on its sample endpoint 0x%02x",
                  device.id.c_str(), endpoint);
  }
#else
  (void)device;
  (void)before;
#endif
}

struct StartAttempt {
  std::unique_ptr<Session> session;
  bool driverEnds = false;
  bridge_usb_counters usbAtStart{};
  StartStage failedAt = StartStage::kNone;
  std::string failure;
};

StartAttempt startAttempt(BridgeDevice& device, const bitoscope_capture& capture, bitoscope_sink* sink) {
  StartAttempt attempt;
  const auto failedAt = [&](StartStage stage, std::string_view what) {
    attempt.failedAt = stage;
    attempt.failure = withSigrokError(what, lastSigrokError());
  };
  clearLastSigrokError();
  if (device.sdi == nullptr) {
    failedAt(StartStage::kNotConnected, "the device is no longer connected");
    return attempt;
  }
  const bridge_usb_counters usbBeforeOpen = usbCounters();
  if (openDevice(device) != SR_OK) {
    failedAt(StartStage::kOpen, "cannot open the device");
    return attempt;
  }
  logRawIo(device, usbBeforeOpen);
  if (std::string what = configureDevice(device, capture, attempt.driverEnds); !what.empty()) {
    failedAt(StartStage::kSetUp, what);
    return attempt;
  }
  auto session =
      std::make_unique<Session>(device, sink, capture.sample_limit, attempt.driverEnds);
  attempt.usbAtStart = usbCounters();
  bool acquisitionStartFailed = false;
  if (std::string what = session->start(acquisitionStartFailed); !what.empty()) {
    failedAt(acquisitionStartFailed ? StartStage::kAcquisitionStart : StartStage::kSetUp, what);
    session.reset();
    return attempt;
  }
  attempt.session = std::move(session);
  return attempt;
}

CoreKeeper makeCoreKeeper(const BridgeDevice& device) {
  bool wanted = device.row != nullptr && device.row->keepCoreAwake;
#ifndef _WIN32
  std::string optIn;
  wanted = wanted && readEnvironment("BITOSCOPE_SIGROK_BRIDGE_KEEP_AWAKE", optIn) && optIn == "1";
#endif
  std::string hookText;
  const bool hookOff = readEnvironment("BITOSCOPE_SIGROK_BRIDGE_TEST_NO_KEEP_AWAKE", hookText) && hookText == "1";
  if (wanted && hookOff) {
    bitoscope_log(BITOSCOPE_LOG_WARNING,
                  "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_NO_KEEP_AWAKE is set: no core is kept awake during "
                  "this capture of %s, though its row asks for one",
                  device.id.c_str());
  } else if (wanted) {
    bitoscope_log(BITOSCOPE_LOG_INFO,
                  "%s: one core is kept awake by a thread of the lowest priority while the capture runs",
                  device.id.c_str());
  }
  return CoreKeeper(wanted && !hookOff);
}

CaptureResult runCapture(BridgeDevice& device, const bitoscope_capture& capture, bitoscope_sink* sink) {
  const CoreKeeper coreKeeper = makeCoreKeeper(device);
  std::unique_lock lock(sigrokMutex());

  StartAttempt attempt = startAttempt(device, capture, sink);
  if (attempt.failedAt != StartStage::kNone) {
    return startFailure(attempt.failedAt, attempt.failure);
  }
  std::unique_ptr<Session> session = std::move(attempt.session);
  const bool driverEnds = attempt.driverEnds;
  const bridge_usb_counters usbAtStart = attempt.usbAtStart;
  bitoscope_log(BITOSCOPE_LOG_INFO, "capturing from %s: channels %s at %llu Hz%s", device.id.c_str(),
                hexMask(capture.channel_mask).c_str(),
                static_cast<unsigned long long>(capture.sample_rate_milli_hz / kMilliHertzPerHertz),
                driverEnds ? "; the driver has the sample limit and ends the session itself" : "");
  lock.unlock();
  session->run();

  if (hasSampleEndpoint(device)) {
    const bridge_usb_counters usbAtEnd = usbCounters();
    const uint64_t completed = usbAtEnd.transfers_completed - usbAtStart.transfers_completed;
    const uint64_t failed = usbAtEnd.transfers_failed - usbAtStart.transfers_failed;
    if (failed == 0) {
      bitoscope_log(BITOSCOPE_LOG_INFO,
                    "capture from %s: %llu USB sample transfers completed, and none was lost", device.id.c_str(),
                    static_cast<unsigned long long>(completed));
    } else {
      bitoscope_log(BITOSCOPE_LOG_WARNING,
                    "capture from %s: %llu USB sample transfers completed, %llu of them failed", device.id.c_str(),
                    static_cast<unsigned long long>(completed), static_cast<unsigned long long>(failed));
    }
  }

  CaptureResult result = session->rules.result(session->sigrokError);
  if (result.succeeded) {
    const int64_t decidedMs = session->decidedMs != 0 ? session->decidedMs : nowMs();
    bitoscope_log(BITOSCOPE_LOG_INFO,
                  "capture from %s is over: %s. %llu samples written in %lld ms, not counting a last write "
                  "that the STOP or the limit cut",
                  device.id.c_str(), endingText(result).c_str(),
                  static_cast<unsigned long long>(session->rules.samplesWritten()),
                  static_cast<long long>(decidedMs - session->startedMs));
  }
  session->close();
  if (result.sessionAbandoned) {
    (void)session.release();
  }
  return result;
}

}

void feedPacket(CaptureRules& rules, bitoscope_sink* sink, uint32_t deviceChannels, const sr_datafeed_packet* packet,
                int64_t nowMs) noexcept {
  try {
    SinkTarget target{sink, deviceChannels};
    if (packet != nullptr) {
      switch (packet->type) {
        case SR_DF_LOGIC:
          if (packet->payload != nullptr) {
            const auto& logic = *static_cast<const sr_datafeed_logic*>(packet->payload);
            rules.deliverLogic(nowMs, static_cast<const uint8_t*>(logic.data), logic.length, logic.unitsize,
                               describeSamples, writeNow, &target);
          }
          break;
        case SR_DF_END:
          rules.endPacket(nowMs);
          break;
        default:
          break;
      }
    }
  } catch (...) {
    rules.internalError();
  }
}

std::string applySampleLimit(BridgeDevice& device, uint64_t captureSampleLimit, bool& driverEnds) {
  driverEnds = false;
  SampleLimitFacts facts;
  std::string stopAtLimit;
  const bool rowStopped =
      readEnvironment("BITOSCOPE_SIGROK_BRIDGE_TEST_STOP_AT_LIMIT", stopAtLimit) && stopAtLimit == "1";
  if (rowStopped) {
    bitoscope_log(BITOSCOPE_LOG_WARNING,
                  "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_STOP_AT_LIMIT is set: the driver is not given the "
                  "capture's sample limit to end by, and the bridge stops the session");
  }
  facts.rowEndsAtSampleLimit = device.row != nullptr && device.row->endsAtSampleLimit && !rowStopped;
  facts.captureSampleLimit = captureSampleLimit;
  facts.settable = can(device, SR_CONF_LIMIT_SAMPLES, kCanSet);
  facts.readable = readUint64(device, SR_CONF_LIMIT_SAMPLES, facts.inForce);
  std::string testLimitText;
  const bool hasTestLimit = readEnvironment("BITOSCOPE_SIGROK_BRIDGE_TEST_LIMIT_SAMPLES", testLimitText);
  facts.testLimit = testLimitSamplesFromText(hasTestLimit ? testLimitText.c_str() : nullptr);

  const SampleLimitPlan plan = planSampleLimit(facts);
  switch (plan.action) {
    case SampleLimitPlan::Action::kRefuse:
      return facts.testLimit != 0 ? std::string("TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_LIMIT_SAMPLES: the "
                                                "device cannot be given a sample limit")
                                  : limitNotCleared("sample limit", plan.samples);
    case SampleLimitPlan::Action::kSet: {
      GVariant* limit = g_variant_new_uint64(plan.samples);
      if (sr_config_set(device.sdi, nullptr, SR_CONF_LIMIT_SAMPLES, limit) != SR_OK) {
        return "cannot give the device its sample limit of " + std::to_string(plan.samples);
      }
      uint64_t taken = 0;
      if (readUint64(device, SR_CONF_LIMIT_SAMPLES, taken) && taken != plan.samples) {
        return "the device took a sample limit of " + std::to_string(taken) + " in place of " +
               std::to_string(plan.samples);
      }
      if (facts.testLimit != 0) {
        bitoscope_log(BITOSCOPE_LOG_WARNING,
                      "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_LIMIT_SAMPLES: libsigrok's sample limit is %llu",
                      static_cast<unsigned long long>(plan.samples));
      }
      break;
    }
    case SampleLimitPlan::Action::kLeave:
      break;
  }
  if (std::string limit = timeLimitInForce(device); !limit.empty()) {
    return limit;
  }
  driverEnds = plan.driverEnds;
  return {};
}

namespace {

std::atomic<bool>& lostReported() {
  static std::atomic<bool> reported{false};
  return reported;
}

DeviceState probeDevice(BridgeDevice& device, std::string& reopenError) {
  DeviceProbe probe;
  {
    std::lock_guard lock(sigrokMutex());
    closeDevice(device);
    clearLastSigrokError();
    probe.reopened = openDevice(device) == SR_OK;
    if (!probe.reopened) {
      reopenError = lastSigrokError();
    }
  }
  if (!probe.reopened) {
    std::lock_guard lock(sigrokMutex());
    const std::optional<bool> present = foundAtItsPlace(device);
    probe.hasBusId = !device.sigrokConn.empty();
    probe.busKnown = present.has_value();
    probe.onBus = present.value_or(true);
  }
  return deviceStateAfterFailure(probe);
}

}

int prepareCapture(void* session, const bitoscope_capture* capture, bitoscope_sink* sink, bitoscope_reply* reply) {
  (void)sink;
  auto* device = static_cast<BridgeDevice*>(session);
  StartRequest request;
  request.buffered = capture->buffered != 0;
  request.channelMask = capture->channel_mask;
  request.sampleRateMilliHz = capture->sample_rate_milli_hz;
  {
    std::lock_guard lock(sigrokMutex());
    request.deviceConnected = device->sdi != nullptr;
    request.row = device->row;
  }
  request.sessionAbandoned = device->capturing.load();
  const std::string refusal = startRefusal(request);
  if (!refusal.empty()) {
    bitoscope_reply_message(reply, refusal.c_str());
    return BITOSCOPE_ERROR;
  }
  return BITOSCOPE_OK;
}

void captureSamples(void* session, const bitoscope_capture* capture, bitoscope_sink* sink) {
  BridgeDevice& device = *static_cast<BridgeDevice*>(session);
  struct Capturing {
    explicit Capturing(BridgeDevice& capturedDevice) : device(capturedDevice) { device.capturing = true; }
    ~Capturing() {
      if (!keep) {
        device.capturing = false;
      }
    }
    BridgeDevice& device;
    bool keep = false;
  } capturing(device);

  const CaptureResult result = runCapture(device, *capture, sink);
  capturing.keep = result.sessionAbandoned;

  DeviceState state = DeviceState::kOpen;
  std::string reopenError;
  if (deviceProbedAfter(result)) {
    state = probeDevice(device, reopenError);
  }

  for (const EndStep& step : endSteps(result, device.name, state, reopenError, lostReported().load())) {
    switch (step.kind) {
      case EndStep::Kind::kDeviceLost:
        bitoscope_device_gone(nullptr, step.text.c_str());
        lostReported() = true;
        break;
      case EndStep::Kind::kFailCapture:
        bitoscope_fail_capture(sink, step.text.c_str());
        break;
      case EndStep::Kind::kLogError:
        bitoscope_log(BITOSCOPE_LOG_ERROR, "%s", step.text.c_str());
        break;
    }
  }
}

}
