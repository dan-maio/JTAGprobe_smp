/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TUSB_EDPT_HANDLER_H
#define TUSB_EDPT_HANDLER_H

#include "tusb.h"

#include "device/usbd_pvt.h"
#include "DAP_config.h"

#define DAP_INTERFACE_SUBCLASS 0x00
#define DAP_INTERFACE_PROTOCOL 0x00

/*
 * A single-producer, single-consumer ring of DAP packets, shared by the two
 * cores. wptr and rptr are free-running counters (never wrapped): the ring
 * holds wptr - rptr packets, and slot n is data[n % DAP_PACKET_COUNT].
 * Only the producer writes wptr, only the consumer writes rptr.
 */
typedef struct {
	uint8_t data[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
	uint16_t resp_len[DAP_PACKET_COUNT];
	volatile uint32_t wptr;
	volatile uint32_t rptr;
} buffer_t;

/* Main DAP loop (the "dap" thread, core 1) */
void *dap_thread(void *ptr);

/* Called with usb_mutex held, by the "usb" thread (core 0) after tud_task()
 * or by the DAP thread (core 1) when its try-lock succeeds: starts the IN
 * transfer of the next DAP response, and re-arms the OUT endpoint once the
 * DAP thread has freed a request slot. */
void dap_edpt_service(void);

/* True while DAP packets are in flight or moved within the last USB_SPIN_US:
 * the "usb" thread then polls tud_task() instead of sleeping. */
bool dap_edpt_active(void);

/* DAP vendor command 0x80 <sel>: writes field <sel> (0..11) of the
 * per-stage packet timing into response as a little-endian uint32 and
 * returns 4; sel 0xFF clears the counters. */
uint32_t dap_stats_report(uint8_t *response, uint8_t sel);

/* Endpoint Handling */
void dap_edpt_init(void);
uint16_t dap_edpt_open(uint8_t __unused rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len);
bool dap_edpt_control_xfer_cb(uint8_t __unused rhport, uint8_t stage,  tusb_control_request_t const *request);
bool dap_edpt_xfer_cb(uint8_t __unused rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);

/* Helper Functions */
bool buffer_full(buffer_t *buffer);
bool buffer_empty(buffer_t *buffer);

#endif
