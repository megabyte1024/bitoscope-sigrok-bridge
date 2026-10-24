/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sigrok_context.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include <glib.h>

#include <bridge_usb/bridge_usb.h>

#include "bitoscope_driver.h"

#include "listing_rules.h"
#include "usb_id_table.h"

namespace sigrok_bridge {

namespace {

constexpr size_t kLineBytes = 1024;

struct LastError {
  std::mutex mutex;
  char text[kLineBytes] = "";
};

LastError& lastError() {
  static LastError instance;
  return instance;
}

struct ActiveLineWatch {
  std::mutex mutex;
  const char* line = nullptr;
  bool seen = false;
  int chosenLevel = SR_LOG_SPEW;
};

ActiveLineWatch& activeLineWatch() {
  static ActiveLineWatch instance;
  return instance;
}

bool noteWatchedLineAndAllowForward(int level, const char* line) {
  ActiveLineWatch& watch = activeLineWatch();
  std::lock_guard lock(watch.mutex);
  if (watch.line == nullptr) {
    return true;
  }
  if (std::strcmp(line, watch.line) == 0) {
    watch.seen = true;
  }
  return level <= watch.chosenLevel;
}

int bitoscopeLevel(int sigrokLevel) {
  switch (severityOfSigrokLevel(sigrokLevel)) {
    case LogSeverity::kError:
      return BITOSCOPE_LOG_ERROR;
    case LogSeverity::kWarning:
      return BITOSCOPE_LOG_WARNING;
    case LogSeverity::kInfo:
      return BITOSCOPE_LOG_INFO;
    case LogSeverity::kDebug:
      break;
  }
  return BITOSCOPE_LOG_DEBUG;
}

int forwardLog(void* data, int level, const char* format, va_list args) noexcept {
  (void)data;
  try {
    char line[kLineBytes];
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    if (length < 0) {
      std::snprintf(line, sizeof(line), "(a log line that could not be formatted: %s)", format);
    }
    size_t size = std::char_traits<char>::length(line);
    while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r' || line[size - 1] == ' ')) {
      line[--size] = '\0';
    }
    if (isProblemLevel(level)) {
      LastError& last = lastError();
      std::lock_guard lock(last.mutex);
      std::memcpy(last.text, line, size + 1);
    }
    if (noteWatchedLineAndAllowForward(level, line)) {
      bitoscope_log(bitoscopeLevel(level), "libsigrok: %s", line);
    }
  } catch (...) {
  }
  return 0;
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

#ifdef _WIN32
std::filesystem::path executableDirectory() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
      return {};
    }
    if (length < path.size()) {
      path.resize(length);
      break;
    }
    path.resize(path.size() * 2);
  }
  return std::filesystem::path(path).parent_path();
}
#else
std::filesystem::path executableDirectory() {
  std::error_code error;
  const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path() : path.parent_path();
}
#endif

std::filesystem::path systemFirmwareDirectory() {
#ifdef _WIN32
  return L"C:\\Program Files (x86)\\sigrok\\PulseView\\share\\sigrok-firmware";
#else
  std::error_code ignored;
  for (const char* candidate : {"/usr/local/share/sigrok-firmware", "/usr/share/sigrok-firmware"}) {
    if (std::filesystem::is_directory(candidate, ignored)) {
      return candidate;
    }
  }
  return "/usr/share/sigrok-firmware";
#endif
}

void chooseFirmwareDirectory() {
  std::string userValue;
  const bool userSetIt = readEnvironment("SIGROK_FIRMWARE_DIR", userValue);
  const std::filesystem::path beside = executableDirectory() / "sigrok-firmware";
  const std::filesystem::path systemDirectory = systemFirmwareDirectory();
  std::error_code ignored;
  const std::string choice =
      firmwareDirectoryChoice(userSetIt, utf8(beside), std::filesystem::is_directory(beside, ignored),
                              utf8(systemDirectory), std::filesystem::is_directory(systemDirectory, ignored));
  if (userSetIt) {
    bitoscope_log(BITOSCOPE_LOG_INFO, "firmware directory from SIGROK_FIRMWARE_DIR: %s", userValue.c_str());
  } else if (choice.empty()) {
    bitoscope_log(BITOSCOPE_LOG_INFO, "no sigrok-firmware directory found: units that need firmware fail");
  } else {
    g_setenv("SIGROK_FIRMWARE_DIR", choice.c_str(), TRUE);
    bitoscope_log(BITOSCOPE_LOG_INFO, "firmware directory: %s", choice.c_str());
  }
}

}

std::recursive_mutex& sigrokMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

#ifdef _WIN32
bool readEnvironment(const char* name, std::string& value) {
  char buffer[512];
  const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (length == 0) {
    return false;
  }
  if (length < sizeof(buffer)) {
    value.assign(buffer, length);
    return true;
  }
  std::string large(length, '\0');
  const DWORD copied = GetEnvironmentVariableA(name, large.data(), length);
  if (copied == 0 || copied >= length) {
    return false;
  }
  large.resize(copied);
  value = std::move(large);
  return true;
}
#else
bool readEnvironment(const char* name, std::string& value) {
  const char* text = std::getenv(name);
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  value = text;
  return true;
}
#endif

sr_context* sigrokContext() {
  std::lock_guard lock(sigrokMutex());
  static bool tried = false;
  static sr_context* context = nullptr;
  if (tried) {
    return context;
  }
  tried = true;

  sr_log_callback_set(forwardLog, nullptr);
  std::string levelText;
  const bool hasLevel = readEnvironment("BITOSCOPE_SIGROK_BRIDGE_LOGLEVEL", levelText);
  sr_log_loglevel_set(sigrokLogLevelFromText(hasLevel ? levelText.c_str() : nullptr));
  chooseFirmwareDirectory();
  setUpUsbShim();

  const int status = sr_init(&context);
  if (status != SR_OK) {
    bitoscope_log(BITOSCOPE_LOG_ERROR, "sr_init failed: %s", sr_strerror(status));
    context = nullptr;
  }
  return context;
}

namespace {

std::atomic<bool>& rawIoOffForTest() {
  static std::atomic<bool> off{false};
  return off;
}

}

bool usbRawIoOffForTest() { return rawIoOffForTest().load(); }

bool setUpUsbShim() {
  if (bridge_usb_api_version() != BRIDGE_USB_API_VERSION) {
    bitoscope_log(BITOSCOPE_LOG_ERROR,
                  "the USB shim beside the driver is version %d and the bridge was built for version %d: "
                  "USB captures will not work until both are from one build",
                  bridge_usb_api_version(), BRIDGE_USB_API_VERSION);
    return false;
  }
#ifndef _WIN32
  if (!bridge_usb_is_interposing()) {
    bitoscope_log(BITOSCOPE_LOG_ERROR,
                  "libusb_submit_transfer() is not the USB shim's in this process (libusb comes before "
                  "libbridge_usb.so in the lookup order): failed USB transfers would go unseen");
    return false;
  }
#endif
  bool rawIoOff = false;
#ifdef _WIN32
  std::string hook;
  rawIoOff = readEnvironment("BITOSCOPE_SIGROK_BRIDGE_TEST_NO_RAW_IO", hook) && hook == "1";
#endif
  rawIoOffForTest() = rawIoOff;
  if (rawIoOff) {
    bitoscope_log(BITOSCOPE_LOG_WARNING,
                  "TEST HOOK BITOSCOPE_SIGROK_BRIDGE_TEST_NO_RAW_IO is set: WinUSB RAW_IO is NOT switched on "
                  "for the devices' sample endpoints, and their captures will lose samples");
  }
  bool ok = true;
  for (const ShimEndpoint& one : shimEndpoints(driverTable())) {
    if (bridge_usb_watch_endpoint(one.vendorId, one.productId, one.endpoint, rawIoOff ? 0 : 1) != 0) {
      bitoscope_log(BITOSCOPE_LOG_ERROR,
                    "the USB shim refused endpoint 0x%02x of %04x:%04x: captures of that device will lose "
                    "samples or fail",
                    one.endpoint, one.vendorId, one.productId);
      ok = false;
    }
  }
  return ok;
}

std::string lastSigrokError() {
  LastError& last = lastError();
  std::lock_guard lock(last.mutex);
  return last.text;
}

void clearLastSigrokError() {
  LastError& last = lastError();
  std::lock_guard lock(last.mutex);
  last.text[0] = '\0';
}

SigrokLineWatch::SigrokLineWatch(const char* line) {
  if (line == nullptr) {
    return;
  }
  previousLevel_ = sr_log_loglevel_get();
  {
    ActiveLineWatch& watch = activeLineWatch();
    std::lock_guard lock(watch.mutex);
    if (watch.line != nullptr) {
      return;
    }
    watch.line = line;
    watch.seen = false;
    watch.chosenLevel = previousLevel_;
  }
  watching_ = true;
  if (previousLevel_ < SR_LOG_INFO) {
    // WHY: sr_log() drops a line above its level before any callback sees it, and the watched lines are INFO.
    sr_log_loglevel_set(SR_LOG_INFO);
  }
}

SigrokLineWatch::~SigrokLineWatch() {
  if (!watching_) {
    return;
  }
  if (previousLevel_ < SR_LOG_INFO) {
    sr_log_loglevel_set(previousLevel_);
  }
  ActiveLineWatch& watch = activeLineWatch();
  std::lock_guard lock(watch.mutex);
  watch.line = nullptr;
}

bool SigrokLineWatch::seen() const {
  if (!watching_) {
    return false;
  }
  ActiveLineWatch& watch = activeLineWatch();
  std::lock_guard lock(watch.mutex);
  return watch.seen;
}

}
