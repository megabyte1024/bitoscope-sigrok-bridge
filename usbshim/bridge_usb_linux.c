/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb-1.0/libusb.h>

#include "bridge_usb.h"

#define MAX_ENDPOINTS 64

struct watched_endpoint {
	uint16_t vendor_id;
	uint16_t product_id;
	uint8_t endpoint;
	int raw_io;
};

static pthread_mutex_t shim_lock = PTHREAD_MUTEX_INITIALIZER;

static struct watched_endpoint endpoints[MAX_ENDPOINTS];
static size_t endpoint_count;

static uint64_t transfers_completed;
static uint64_t transfers_failed;

static void complain(const char *what, const char *detail)
{
	fprintf(stderr, "bridge_usb: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

static void count(uint64_t *counter)
{
	__atomic_add_fetch(counter, 1, __ATOMIC_SEQ_CST);
}

static uint64_t read_count(uint64_t *counter)
{
	return __atomic_load_n(counter, __ATOMIC_SEQ_CST);
}

typedef int (*submit_fn)(struct libusb_transfer *transfer);

static submit_fn real_submit_cached;

static submit_fn as_submit_fn(void *symbol)
{
	submit_fn function;

	_Static_assert(sizeof function == sizeof symbol, "function and object pointers differ in size");
	memcpy(&function, &symbol, sizeof function);
	return function;
}

extern int LIBUSB_CALL shim_own_submit_transfer(struct libusb_transfer *transfer)
	__attribute__((visibility("hidden")));

static submit_fn real_submit(void)
{
	submit_fn function = __atomic_load_n(&real_submit_cached, __ATOMIC_ACQUIRE);

	if (function)
		return function;
	function = as_submit_fn(dlsym(RTLD_NEXT, "libusb_submit_transfer"));
	if (!function) {
		void *module = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);

		if (module)
			function = as_submit_fn(dlsym(module, "libusb_submit_transfer"));
	}
	if (!function || function == shim_own_submit_transfer) {
		complain("libusb_submit_transfer", "the real function cannot be found");
		abort();
	}
	__atomic_store_n(&real_submit_cached, function, __ATOMIC_RELEASE);
	return function;
}

int bridge_usb_api_version(void)
{
	return BRIDGE_USB_API_VERSION;
}

int bridge_usb_watch_endpoint(uint16_t vendor_id, uint16_t product_id, uint8_t endpoint, int raw_io)
{
	size_t i;
	int result = 0;

	if (!(endpoint & LIBUSB_ENDPOINT_IN) || !(endpoint & LIBUSB_ENDPOINT_ADDRESS_MASK))
		return -1;

	pthread_mutex_lock(&shim_lock);
	for (i = 0; i < endpoint_count; i++) {
		if (endpoints[i].vendor_id == vendor_id && endpoints[i].product_id == product_id &&
		    endpoints[i].endpoint == endpoint)
			break;
	}
	if (i == MAX_ENDPOINTS) {
		result = -1;
	} else {
		endpoints[i].vendor_id = vendor_id;
		endpoints[i].product_id = product_id;
		endpoints[i].endpoint = endpoint;
		endpoints[i].raw_io = raw_io != 0;
		if (i == endpoint_count)
			endpoint_count++;
	}
	pthread_mutex_unlock(&shim_lock);
	return result;
}

int bridge_usb_endpoint_policy(uint16_t vendor_id, uint16_t product_id, uint8_t endpoint)
{
	size_t i;
	int policy = -1;

	pthread_mutex_lock(&shim_lock);
	for (i = 0; i < endpoint_count; i++) {
		if (endpoints[i].vendor_id == vendor_id && endpoints[i].product_id == product_id &&
		    endpoints[i].endpoint == endpoint)
			policy = endpoints[i].raw_io;
	}
	pthread_mutex_unlock(&shim_lock);
	return policy;
}

int bridge_usb_transfer_failed(int status)
{
	switch (status) {
	case LIBUSB_TRANSFER_COMPLETED:
	case LIBUSB_TRANSFER_TIMED_OUT:
	case LIBUSB_TRANSFER_CANCELLED:
	case LIBUSB_TRANSFER_NO_DEVICE:
		return 0;
	default:
		return 1;
	}
}

uint64_t bridge_usb_failed_transfer_count(void)
{
	return read_count(&transfers_failed);
}

void bridge_usb_read_counters(struct bridge_usb_counters *counters)
{
	if (!counters)
		return;
	counters->raw_io_enabled = 0;
	counters->raw_io_failed = 0;
	counters->transfers_completed = read_count(&transfers_completed);
	counters->transfers_failed = read_count(&transfers_failed);
}

static int is_watched(libusb_device_handle *handle, uint8_t endpoint)
{
	struct libusb_device_descriptor descriptor;
	size_t i;
	int watched = 0;

	if (!handle || libusb_get_device_descriptor(libusb_get_device(handle), &descriptor) != 0)
		return 0;
	pthread_mutex_lock(&shim_lock);
	for (i = 0; i < endpoint_count && !watched; i++) {
		watched = endpoints[i].vendor_id == descriptor.idVendor &&
			  endpoints[i].product_id == descriptor.idProduct && endpoints[i].endpoint == endpoint;
	}
	pthread_mutex_unlock(&shim_lock);
	return watched;
}

struct wrapped_transfer {
	struct libusb_transfer *transfer;
	libusb_transfer_cb_fn driver_callback;
};

static struct wrapped_transfer *wrapped;
static size_t wrapped_count;
static size_t wrapped_capacity;

static void LIBUSB_CALL transfer_done(struct libusb_transfer *transfer);

static int carries_shim_callback(const struct libusb_transfer *transfer)
{
	return transfer->callback == transfer_done;
}

static struct wrapped_transfer *find_wrapped_locked(struct libusb_transfer *transfer)
{
	size_t i;

	for (i = 0; i < wrapped_count; i++) {
		if (wrapped[i].transfer == transfer)
			return &wrapped[i];
	}
	return NULL;
}

static int wrap_transfer(struct libusb_transfer *transfer)
{
	struct wrapped_transfer *entry;
	int result = 0;

	pthread_mutex_lock(&shim_lock);
	entry = find_wrapped_locked(transfer);
	if (!entry) {
		if (wrapped_count == wrapped_capacity) {
			size_t capacity = wrapped_capacity ? wrapped_capacity * 2 : 64;
			struct wrapped_transfer *grown = realloc(wrapped, capacity * sizeof(*wrapped));

			if (grown) {
				wrapped = grown;
				wrapped_capacity = capacity;
			}
		}
		if (wrapped_count < wrapped_capacity) {
			entry = &wrapped[wrapped_count++];
			entry->transfer = transfer;
			entry->driver_callback = NULL;
		}
	}
	if (!entry) {
		result = -1;
	} else if (!carries_shim_callback(transfer)) {
		entry->driver_callback = transfer->callback;
		transfer->callback = transfer_done;
	}
	pthread_mutex_unlock(&shim_lock);
	return result;
}

static libusb_transfer_cb_fn unwrap_transfer(struct libusb_transfer *transfer, int *was_wrapped)
{
	struct wrapped_transfer *entry;
	libusb_transfer_cb_fn driver_callback = NULL;

	pthread_mutex_lock(&shim_lock);
	entry = find_wrapped_locked(transfer);
	*was_wrapped = entry != NULL;
	if (entry) {
		driver_callback = entry->driver_callback;
		*entry = wrapped[--wrapped_count];
		transfer->callback = driver_callback;
	}
	pthread_mutex_unlock(&shim_lock);
	return driver_callback;
}

static void LIBUSB_CALL transfer_done(struct libusb_transfer *transfer)
{
	libusb_transfer_cb_fn driver_callback;
	int was_wrapped;

	driver_callback = unwrap_transfer(transfer, &was_wrapped);
	if (!was_wrapped) {
		count(&transfers_failed);
		complain("a transfer completed that the shim did not submit", NULL);
		return;
	}
	count(&transfers_completed);
	if (bridge_usb_transfer_failed(transfer->status))
		count(&transfers_failed);
	if (driver_callback)
		driver_callback(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
	int was_wrapped;
	int result;

	if (transfer->type != LIBUSB_TRANSFER_TYPE_BULK || !is_watched(transfer->dev_handle, transfer->endpoint))
		return real_submit()(transfer);

	if (wrap_transfer(transfer) != 0)
		return LIBUSB_ERROR_NO_MEM;
	result = real_submit()(transfer);
	if (result != 0)
		unwrap_transfer(transfer, &was_wrapped);
	return result;
}

extern int LIBUSB_CALL shim_own_submit_transfer(struct libusb_transfer *transfer)
	__attribute__((alias("libusb_submit_transfer"), visibility("hidden")));

int bridge_usb_is_interposing(void)
{
	/* WHY: libusb_submit_transfer here binds to libusb when libusb comes first; the alias cannot. */
	return as_submit_fn(dlsym(RTLD_DEFAULT, "libusb_submit_transfer")) == shim_own_submit_transfer;
}

int bridge_usb_test_wrap_transfer(struct libusb_transfer *transfer)
{
	return wrap_transfer(transfer);
}
