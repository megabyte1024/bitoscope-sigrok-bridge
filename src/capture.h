/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_CAPTURE_H
#define SIGROK_BRIDGE_CAPTURE_H

#include <cstdint>
#include <string>

#include "bitoscope_driver.h"

#include "capture_rules.h"

struct sr_datafeed_packet;

namespace sigrok_bridge {

struct BridgeDevice;

int prepareCapture(void* session, const bitoscope_capture* capture, bitoscope_sink* sink, bitoscope_reply* reply);

void captureSamples(void* session, const bitoscope_capture* capture, bitoscope_sink* sink);

std::string applySampleLimit(BridgeDevice& device, uint64_t captureSampleLimit, bool& driverEnds);

void feedPacket(CaptureRules& rules, bitoscope_sink* sink, uint32_t deviceChannels, const sr_datafeed_packet* packet,
                int64_t nowMs) noexcept;

}

#endif
