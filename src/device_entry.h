/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SIGROK_BRIDGE_DEVICE_ENTRY_H
#define SIGROK_BRIDGE_DEVICE_ENTRY_H

#include <string>

#include "bitoscope_driver.h"

namespace sigrok_bridge {

struct BridgeDevice;

int describeDevice(BridgeDevice& device, bitoscope_device* listed, bitoscope_reply* reply, std::string& whyNot);

}

#endif
