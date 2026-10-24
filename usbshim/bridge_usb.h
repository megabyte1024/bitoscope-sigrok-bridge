/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BRIDGE_USB_H
#define BRIDGE_USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_USB_API_VERSION 1
int bridge_usb_api_version(void);

#ifndef _WIN32

int bridge_usb_is_interposing(void);
#endif

int bridge_usb_watch_endpoint(uint16_t vendor_id, uint16_t product_id, uint8_t endpoint, int raw_io);

int bridge_usb_endpoint_policy(uint16_t vendor_id, uint16_t product_id, uint8_t endpoint);

int bridge_usb_transfer_failed(int status);

uint64_t bridge_usb_failed_transfer_count(void);

struct bridge_usb_counters {
  uint64_t raw_io_enabled;
  uint64_t raw_io_failed;
  uint64_t transfers_completed;
  uint64_t transfers_failed;
};
void bridge_usb_read_counters(struct bridge_usb_counters *counters);

struct libusb_transfer;
int bridge_usb_test_wrap_transfer(struct libusb_transfer *transfer);

#ifdef __cplusplus
}
#endif

#endif
