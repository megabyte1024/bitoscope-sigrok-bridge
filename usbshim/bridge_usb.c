/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libusb.h>

#include "bridge_usb.h"

#define REAL_FUNCTIONS(X) \
	X(libusb_exit) \
	X(libusb_close) \
	X(libusb_claim_interface) \
	X(libusb_get_device) \
	X(libusb_get_device_descriptor) \
	X(libusb_endpoint_supports_raw_io) \
	X(libusb_endpoint_set_raw_io) \
	X(libusb_submit_transfer)

struct real_functions {
#define X(name) __typeof__(name) name;
	REAL_FUNCTIONS(X)
#undef X
};

static struct real_functions real_table;
static INIT_ONCE real_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_real(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	HMODULE module = LoadLibraryW(L"libusb-1.0.dll");

	(void)once;
	(void)parameter;
	(void)context;
	if (!module)
		return FALSE;
#define X(name) \
	if (!(real_table.name = (__typeof__(name))GetProcAddress(module, #name))) \
		return FALSE;
	REAL_FUNCTIONS(X)
#undef X
	return TRUE;
}

static void complain(const char *what, const char *detail);

static const struct real_functions *real(void)
{
	if (!InitOnceExecuteOnce(&real_once, load_real, NULL, NULL)) {
		complain("libusb-1.0.dll", "cannot be loaded, or lacks a function the shim calls");
		abort();
	}
	return &real_table;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static SRWLOCK shim_lock = SRWLOCK_INIT;

static void complain(const char *what, const char *detail)
{
	fprintf(stderr, "bridge_usb: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

#define MAX_ENDPOINTS 64

struct watched_endpoint {
	uint16_t vendor_id;
	uint16_t product_id;
	uint8_t endpoint;
	int raw_io;
};

static struct watched_endpoint endpoints[MAX_ENDPOINTS];
static size_t endpoint_count;

static uint64_t raw_io_enabled;
static uint64_t raw_io_failed;
static uint64_t transfers_completed;
static uint64_t transfers_failed;

static void count(uint64_t *counter)
{
	_InterlockedIncrement64((volatile LONG64 *)counter);
}

static uint64_t read_count(uint64_t *counter)
{
	return (uint64_t)_InterlockedCompareExchange64((volatile LONG64 *)counter, 0, 0);
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

	AcquireSRWLockExclusive(&shim_lock);
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
	ReleaseSRWLockExclusive(&shim_lock);
	return result;
}

int bridge_usb_endpoint_policy(uint16_t vendor_id, uint16_t product_id, uint8_t endpoint)
{
	size_t i;
	int policy = -1;

	AcquireSRWLockShared(&shim_lock);
	for (i = 0; i < endpoint_count; i++) {
		if (endpoints[i].vendor_id == vendor_id && endpoints[i].product_id == product_id &&
		    endpoints[i].endpoint == endpoint)
			policy = endpoints[i].raw_io;
	}
	ReleaseSRWLockShared(&shim_lock);
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
	counters->raw_io_enabled = read_count(&raw_io_enabled);
	counters->raw_io_failed = read_count(&raw_io_failed);
	counters->transfers_completed = read_count(&transfers_completed);
	counters->transfers_failed = read_count(&transfers_failed);
}

#define TICK_MS 1
#define POLLIN_SAME_VALUE_AS_LIBUSB_AND_GLIB_G_IO_IN 0x0001

#define MAX_CONTEXTS 16

struct context_tick {
	int used;
	libusb_context *context;
	HANDLE timer;
	unsigned int pollers;
};

static struct context_tick ticks[MAX_CONTEXTS];

static struct context_tick *find_tick_locked(libusb_context *context)
{
	size_t i;

	for (i = 0; i < MAX_CONTEXTS; i++) {
		if (ticks[i].used && ticks[i].context == context)
			return &ticks[i];
	}
	return NULL;
}

static int handle_survives_int_round_trip(HANDLE handle)
{
	return (HANDLE)(intptr_t)(int)(intptr_t)handle == handle;
}

static HANDLE create_high_resolution_timer_or_system_timer(void)
{
	HANDLE timer;

	timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!timer)
		timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	return timer;
}

static HANDLE count_poller_and_get_timer(libusb_context *context)
{
	struct context_tick *tick;
	HANDLE timer = NULL;
	size_t i;

	AcquireSRWLockExclusive(&shim_lock);
	tick = find_tick_locked(context);
	if (!tick) {
		for (i = 0; i < MAX_CONTEXTS && !tick; i++) {
			if (!ticks[i].used)
				tick = &ticks[i];
		}
		if (tick) {
			tick->timer = create_high_resolution_timer_or_system_timer();
			if (tick->timer && !handle_survives_int_round_trip(tick->timer)) {
				CloseHandle(tick->timer);
				tick->timer = NULL;
			}
			if (tick->timer) {
				tick->used = 1;
				tick->context = context;
				tick->pollers = 0;
			} else {
				tick = NULL;
			}
		}
	}
	if (tick) {
		if (tick->pollers == 0) {
			LARGE_INTEGER due;

			due.QuadPart = -10000LL * TICK_MS;
			if (!SetWaitableTimer(tick->timer, &due, TICK_MS, NULL, NULL, FALSE))
				tick = NULL;
		}
		if (tick) {
			tick->pollers++;
			timer = tick->timer;
		}
	}
	ReleaseSRWLockExclusive(&shim_lock);
	return timer;
}

static void uncount_poller(libusb_context *context)
{
	struct context_tick *tick;

	AcquireSRWLockExclusive(&shim_lock);
	tick = find_tick_locked(context);
	if (tick && tick->pollers > 0 && --tick->pollers == 0)
		CancelWaitableTimer(tick->timer);
	ReleaseSRWLockExclusive(&shim_lock);
}

static void forget_tick(libusb_context *context)
{
	struct context_tick *tick;

	if (!context)
		return;
	AcquireSRWLockExclusive(&shim_lock);
	tick = find_tick_locked(context);
	if (tick) {
		CancelWaitableTimer(tick->timer);
		CloseHandle(tick->timer);
		tick->used = 0;
		tick->timer = NULL;
		tick->pollers = 0;
	}
	ReleaseSRWLockExclusive(&shim_lock);
}

struct poll_list {
	const struct libusb_pollfd *entries[2];
	struct libusb_pollfd fd;
};

const struct libusb_pollfd ** LIBUSB_CALL libusb_get_pollfds(libusb_context *ctx)
{
	struct poll_list *list;
	HANDLE timer;

	list = calloc(1, sizeof(*list));
	if (!list)
		return NULL;
	timer = count_poller_and_get_timer(ctx);
	if (!timer) {
		complain("libusb_get_pollfds", "no waitable timer for the context");
		free(list);
		return NULL;
	}
	list->fd.fd = (int)(intptr_t)timer;
	list->fd.events = POLLIN_SAME_VALUE_AS_LIBUSB_AND_GLIB_G_IO_IN;
	list->entries[0] = &list->fd;
	list->entries[1] = NULL;
	return list->entries;
}

void LIBUSB_CALL libusb_free_pollfds(const struct libusb_pollfd **pollfds)
{
	free((void *)pollfds);
}

void LIBUSB_CALL libusb_set_pollfd_notifiers(libusb_context *ctx, libusb_pollfd_added_cb added_cb,
		libusb_pollfd_removed_cb removed_cb, void *user_data)
{
	(void)user_data;
	if (!added_cb && !removed_cb)
		uncount_poller(ctx);
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
	forget_tick(ctx);
	real()->libusb_exit(ctx);
}

#define MAX_HANDLES 64

struct watched_handle {
	libusb_device_handle *handle;
	uint8_t endpoint;
};

static struct watched_handle handles[MAX_HANDLES];
static size_t handle_count;

static int handle_is_watched_locked(libusb_device_handle *handle, uint8_t endpoint)
{
	size_t i;

	for (i = 0; i < handle_count; i++) {
		if (handles[i].handle == handle && handles[i].endpoint == endpoint)
			return 1;
	}
	return 0;
}

static void watch_handle(libusb_device_handle *handle, uint8_t endpoint)
{
	AcquireSRWLockExclusive(&shim_lock);
	if (!handle_is_watched_locked(handle, endpoint)) {
		if (handle_count < MAX_HANDLES) {
			handles[handle_count].handle = handle;
			handles[handle_count].endpoint = endpoint;
			handle_count++;
		} else {
			complain("too many open devices", "the transfers of one more are not watched");
		}
	}
	ReleaseSRWLockExclusive(&shim_lock);
}

static void forget_handle(libusb_device_handle *handle)
{
	size_t i = 0;

	AcquireSRWLockExclusive(&shim_lock);
	while (i < handle_count) {
		if (handles[i].handle == handle)
			handles[i] = handles[--handle_count];
		else
			i++;
	}
	ReleaseSRWLockExclusive(&shim_lock);
}

static void enable_raw_io(libusb_device_handle *handle, uint8_t endpoint)
{
	char text[96];
	int result;

	result = real()->libusb_endpoint_supports_raw_io(handle, endpoint);
	if (result == 1) {
		result = real()->libusb_endpoint_set_raw_io(handle, endpoint, 1);
		if (result == 0) {
			count(&raw_io_enabled);
			return;
		}
	}
	count(&raw_io_failed);
	snprintf(text, sizeof(text), "endpoint 0x%02x, libusb said %d", endpoint, result);
	complain("RAW_IO could not be switched on", text);
}

static void after_claim(libusb_device_handle *handle)
{
	struct libusb_device_descriptor descriptor;
	struct watched_endpoint endpoints_of_this_device[MAX_ENDPOINTS];
	size_t endpoints_of_this_device_count = 0;
	size_t i;

	if (real()->libusb_get_device_descriptor(real()->libusb_get_device(handle), &descriptor) != 0)
		return;

	AcquireSRWLockShared(&shim_lock);
	for (i = 0; i < endpoint_count; i++) {
		if (endpoints[i].vendor_id == descriptor.idVendor &&
		    endpoints[i].product_id == descriptor.idProduct)
			endpoints_of_this_device[endpoints_of_this_device_count++] = endpoints[i];
	}
	ReleaseSRWLockShared(&shim_lock);

	for (i = 0; i < endpoints_of_this_device_count; i++) {
		watch_handle(handle, endpoints_of_this_device[i].endpoint);
		if (endpoints_of_this_device[i].raw_io)
			enable_raw_io(handle, endpoints_of_this_device[i].endpoint);
	}
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *dev_handle, int interface_number)
{
	int result;

	result = real()->libusb_claim_interface(dev_handle, interface_number);
	if (result == 0)
		after_claim(dev_handle);
	return result;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *dev_handle)
{
	if (dev_handle)
		forget_handle(dev_handle);
	real()->libusb_close(dev_handle);
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

	AcquireSRWLockExclusive(&shim_lock);
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
	ReleaseSRWLockExclusive(&shim_lock);
	return result;
}

static libusb_transfer_cb_fn unwrap_transfer(struct libusb_transfer *transfer, int *was_wrapped)
{
	struct wrapped_transfer *entry;
	libusb_transfer_cb_fn driver_callback = NULL;

	AcquireSRWLockExclusive(&shim_lock);
	entry = find_wrapped_locked(transfer);
	*was_wrapped = entry != NULL;
	if (entry) {
		driver_callback = entry->driver_callback;
		*entry = wrapped[--wrapped_count];
		transfer->callback = driver_callback;
	}
	ReleaseSRWLockExclusive(&shim_lock);
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
	int watched;
	int was_wrapped;
	int result;

	AcquireSRWLockShared(&shim_lock);
	watched = transfer->type == LIBUSB_TRANSFER_TYPE_BULK &&
		  handle_is_watched_locked(transfer->dev_handle, transfer->endpoint);
	ReleaseSRWLockShared(&shim_lock);
	if (!watched)
		return real()->libusb_submit_transfer(transfer);

	if (wrap_transfer(transfer) != 0)
		return LIBUSB_ERROR_NO_MEM;
	result = real()->libusb_submit_transfer(transfer);
	if (result != 0)
		unwrap_transfer(transfer, &was_wrapped);
	return result;
}

int bridge_usb_test_wrap_transfer(struct libusb_transfer *transfer)
{
	return wrap_transfer(transfer);
}
