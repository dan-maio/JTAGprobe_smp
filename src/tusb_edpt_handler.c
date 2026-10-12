/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "board_pico_config.h"
#include "DAP_config.h"
#include "probe.h"
#include "probe_os.h"

#include <hardware/sync.h>
#include <hardware/timer.h>

/*
 * The DAP requests and responses cross between the cores in two rings:
 *
 *   USBRequestBuffer   produced by the OUT endpoint callback ("usb", core 0),
 *                      consumed by the DAP thread ("dap", core 1);
 *   USBResponseBuffer  produced by the DAP thread (core 1), consumed by the
 *                      IN endpoint ("usb", core 0).
 *
 * Each index has one writer, so the rings need no lock, only a memory barrier
 * between a slot's data and the index that publishes it. Every tinyusb call
 * is made with usb_mutex held: when the USB side has something to do, the DAP
 * thread runs dap_edpt_service() itself if it gets the mutex, as the
 * single-core probe does from its DAP task, and otherwise posts usb_sem so
 * the "usb" thread does it after tud_task().
 */

static uint8_t itf_num;
static uint8_t _rhport;

volatile uint32_t _resp_len;

static uint8_t _out_ep_addr;
static uint8_t _in_ep_addr;

static buffer_t USBRequestBuffer;
static buffer_t USBResponseBuffer;

/* The OUT endpoint was not re-armed because every request slot is full; the
 * "usb" thread re-arms it once the DAP thread has consumed one. */
static volatile bool out_stalled;

/* An IN transfer of a response is in flight ("usb" thread only). */
static bool in_busy;

static uint8_t DAPRequestBuffer[DAP_PACKET_SIZE];
static uint8_t DAPResponseBuffer[DAP_PACKET_SIZE];

/*
 * Per-stage packet timing, read by the host with DAP vendor command 0x80
 * (OpenOCD: `cmsis-dap cmd 0x80 0x00`, or `0x80 0x01` to read and reset).
 * All times are microseconds from the 1 MHz timer, which both cores share.
 * Each field has one writer: OUT/IN fields core 0 (or whoever holds
 * usb_mutex), DAP fields core 1. A reset can race an update; a read is a
 * snapshot, good to a packet.
 */
#define DAP_GAP_IDLE_US 10000u	// an OUT gap longer than this is host idle time

static struct {
	uint32_t packets;	// requests executed
	uint32_t queue_us;	// OUT received -> DAP thread starts it
	uint32_t exec_us;	// DAP_ExecuteCommand()
	uint32_t exec_max_us;
	uint32_t slot_wait_us;	// DAP waiting for a free response slot
	uint32_t inq_us;	// response ready -> IN transfer started
	uint32_t in_us;		// IN transfer started -> IN complete
	uint32_t in_n;
	uint32_t gap_us;	// OUT -> next OUT, when < DAP_GAP_IDLE_US
	uint32_t gap_n;
	uint32_t gap_idle_n;	// gaps >= DAP_GAP_IDLE_US
	uint32_t out_stalls;	// OUT endpoint left unarmed: request ring full
} dap_stats;

static uint32_t t_out[DAP_PACKET_COUNT];	// per request slot
static uint32_t t_resp[DAP_PACKET_COUNT];	// per response slot
static uint32_t t_in_start;
static uint32_t t_prev_out;

// Last time a DAP packet moved, for dap_edpt_active().
static volatile uint32_t t_last_activity;

#define WR_IDX(x) (x.wptr % DAP_PACKET_COUNT)
#define RD_IDX(x) (x.rptr % DAP_PACKET_COUNT)

#define WR_SLOT_PTR(x) &(x.data[WR_IDX(x)][0])
#define RD_SLOT_PTR(x) &(x.data[RD_IDX(x)][0])

bool buffer_full(buffer_t *buffer)
{
	return (buffer->wptr - buffer->rptr) >= DAP_PACKET_COUNT;
}

bool buffer_empty(buffer_t *buffer)
{
	return (buffer->wptr == buffer->rptr);
}

void dap_edpt_init(void) {

}

void dap_edpt_reset(uint8_t __unused rhport)
{
	itf_num = 0;
}

char * dap_cmd_string[] = {
	[ID_DAP_Info               ] = "DAP_Info",
	[ID_DAP_HostStatus         ] = "DAP_HostStatus",
	[ID_DAP_Connect            ] = "DAP_Connect",
	[ID_DAP_Disconnect         ] = "DAP_Disconnect",
	[ID_DAP_TransferConfigure  ] = "DAP_TransferConfigure",
	[ID_DAP_Transfer           ] = "DAP_Transfer",
	[ID_DAP_TransferBlock      ] = "DAP_TransferBlock",
	[ID_DAP_TransferAbort      ] = "DAP_TransferAbort",
	[ID_DAP_WriteABORT         ] = "DAP_WriteABORT",
	[ID_DAP_Delay              ] = "DAP_Delay",
	[ID_DAP_ResetTarget        ] = "DAP_ResetTarget",
	[ID_DAP_SWJ_Pins           ] = "DAP_SWJ_Pins",
	[ID_DAP_SWJ_Clock          ] = "DAP_SWJ_Clock",
	[ID_DAP_SWJ_Sequence       ] = "DAP_SWJ_Sequence",
	[ID_DAP_SWD_Configure      ] = "DAP_SWD_Configure",
	[ID_DAP_SWD_Sequence       ] = "DAP_SWD_Sequence",
	[ID_DAP_JTAG_Sequence      ] = "DAP_JTAG_Sequence",
	[ID_DAP_JTAG_Configure     ] = "DAP_JTAG_Configure",
	[ID_DAP_JTAG_IDCODE        ] = "DAP_JTAG_IDCODE",
	[ID_DAP_SWO_Transport      ] = "DAP_SWO_Transport",
	[ID_DAP_SWO_Mode           ] = "DAP_SWO_Mode",
	[ID_DAP_SWO_Baudrate       ] = "DAP_SWO_Baudrate",
	[ID_DAP_SWO_Control        ] = "DAP_SWO_Control",
	[ID_DAP_SWO_Status         ] = "DAP_SWO_Status",
	[ID_DAP_SWO_ExtendedStatus ] = "DAP_SWO_ExtendedStatus",
	[ID_DAP_SWO_Data           ] = "DAP_SWO_Data",
	[ID_DAP_QueueCommands      ] = "DAP_QueueCommands",
	[ID_DAP_ExecuteCommands    ] = "DAP_ExecuteCommands",
};


uint16_t dap_edpt_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
	_rhport = rhport;

	TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass &&
			DAP_INTERFACE_SUBCLASS == itf_desc->bInterfaceSubClass &&
			DAP_INTERFACE_PROTOCOL == itf_desc->bInterfaceProtocol, 0);

	//  Initialise circular buffer indices
	USBResponseBuffer.wptr = 0;
	USBResponseBuffer.rptr = 0;
	USBRequestBuffer.wptr = 0;
	USBRequestBuffer.rptr = 0;

	// Initialise the endpoint states
	out_stalled = false;
	in_busy = false;

	uint16_t const drv_len = sizeof(tusb_desc_interface_t) + (itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
	TU_VERIFY(max_len >= drv_len, 0);
	itf_num = itf_desc->bInterfaceNumber;

	// Initialising the OUT endpoint

	tusb_desc_endpoint_t *edpt_desc = (tusb_desc_endpoint_t *) (itf_desc + 1);
	uint8_t ep_addr = edpt_desc->bEndpointAddress;

	_out_ep_addr = ep_addr;

	// The OUT endpoint requires a call to usbd_edpt_xfer to initialise the endpoint, giving tinyUSB a buffer to consume when a transfer occurs at the endpoint
	usbd_edpt_open(rhport, edpt_desc);
	usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);

	// Initiliasing the IN endpoint

	edpt_desc++;
	ep_addr = edpt_desc->bEndpointAddress;

	_in_ep_addr = ep_addr;

	// The IN endpoint doesn't need a transfer to initialise it, as this will be done by the main loop of dap_thread
	usbd_edpt_open(rhport, edpt_desc);

	return drv_len;

}

bool dap_edpt_control_xfer_cb(uint8_t __unused rhport, uint8_t stage, tusb_control_request_t const *request)
{
	return false;
}

// Start the IN transfer of the oldest response, if none is in flight.
// "usb" thread (core 0), usb_mutex held.
static void dap_edpt_start_in(void)
{
	if(!in_busy && !buffer_empty(&USBResponseBuffer))
	{
		// The response's data, after its index (written by core 1).
		__dmb();
		in_busy = true;
		t_in_start = time_us_32();
		dap_stats.inq_us += t_in_start - t_resp[RD_IDX(USBResponseBuffer)];
		usbd_edpt_xfer(_rhport, _in_ep_addr, RD_SLOT_PTR(USBResponseBuffer),
			       USBResponseBuffer.resp_len[RD_IDX(USBResponseBuffer)]);
	}
}

// Manage USBRequestBuffer (request) write and USBResponseBuffer (response) read indices.
// Runs inside tud_task(), so in the "usb" thread (core 0), usb_mutex held.
bool dap_edpt_xfer_cb(uint8_t __unused rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	const uint8_t ep_dir = tu_edpt_dir(ep_addr);

	if(ep_dir == TUSB_DIR_IN)
	{
		if(xferred_bytes >= 0u && xferred_bytes <= DAP_PACKET_SIZE)
		{
			// The response went out: free its slot and send the next one.
			uint32_t now = time_us_32();
			dap_stats.in_us += now - t_in_start;
			dap_stats.in_n++;
			t_last_activity = now;
			in_busy = false;
			USBResponseBuffer.rptr++;
			dap_edpt_start_in();

			//  Wake up the DAP thread: it may be waiting for a free response slot
			os_semaphore_post(&dap_sem);
			return true;
		}

		return false;

	} else if(ep_dir == TUSB_DIR_OUT)    {

		if(xferred_bytes >= 0u && xferred_bytes <= DAP_PACKET_SIZE)
		{
			uint32_t now = time_us_32();
			uint32_t gap = now - t_prev_out;
			if (gap < DAP_GAP_IDLE_US) {
				dap_stats.gap_us += gap;
				dap_stats.gap_n++;
			} else {
				dap_stats.gap_idle_n++;
			}
			t_prev_out = now;
			t_last_activity = now;
			t_out[WR_IDX(USBRequestBuffer)] = now;

			// Publish the request to the DAP thread: its data before its index.
			__dmb();
			USBRequestBuffer.wptr++;

			// Queue the next buffer if a slot is free. If not, the "usb"
			// thread re-arms the endpoint when the DAP thread frees one.
			if(!buffer_full(&USBRequestBuffer))
			{
				usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);
			}
			else {
				out_stalled = true;
				dap_stats.out_stalls++;
			}

			//  Wake up the DAP thread
			os_semaphore_post(&dap_sem);
			return true;
		}

		return false;
	}
	else return false;
}

void dap_edpt_service(void)
{
	// A request slot was freed while the OUT endpoint was stalled.
	if(out_stalled && !buffer_full(&USBRequestBuffer))
	{
		out_stalled = false;
		usbd_edpt_xfer(_rhport, _out_ep_addr, WR_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);
	}

	// A response is ready and the IN endpoint is idle.
	dap_edpt_start_in();
}

// How long after the last DAP packet moved the "usb" thread keeps polling
// tud_task() instead of sleeping on usb_sem.
#define USB_SPIN_US 2000u

bool dap_edpt_active(void)
{
	return !buffer_empty(&USBRequestBuffer) || !buffer_empty(&USBResponseBuffer)
		|| (time_us_32() - t_last_activity) < USB_SPIN_US;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

uint32_t dap_stats_report(uint8_t *response, uint8_t sel)
{
	// One field per command, little-endian: OpenOCD's `cmsis-dap cmd`
	// prints only the first four bytes of a reply.
	const uint32_t *f = (const uint32_t *) &dap_stats;
	const uint32_t n = sizeof(dap_stats) / sizeof(uint32_t);
	if (sel == 0xFFu) {
		memset(&dap_stats, 0, sizeof(dap_stats));
		put_u32(response, 0);
	} else if (sel < n) {
		put_u32(response, f[sel]);
	} else {
		put_u32(response, 0xFFFFFFFFu);
	}
	return 4;
}

// How long the DAP thread busy-waits for the next packet, or for a free
// response slot, before it blocks on dap_sem. Core 1 runs nothing else, so
// spinning costs nothing; during a transfer the next packet arrives well
// within this, and the thread no longer pays a sleep, an IPI and two context
// switches per packet. Once the host goes quiet it sleeps as before.
#define DAP_SPIN_US 2000u

// Wait until *counter moves away from seen: spin first, then block. A post
// that lands between the last check and the wait leaves dap_sem at 1, so the
// wait returns at once and the caller checks again.
static void dap_wait_change(volatile uint32_t *counter, uint32_t seen)
{
	uint32_t start = time_us_32();
	while (*counter == seen) {
		if ((time_us_32() - start) >= DAP_SPIN_US) {
			os_semaphore_wait(&dap_sem);
			return;
		}
	}
}

// Start the IN transfer of a new response, or re-arm a stalled OUT endpoint,
// from this core when tinyusb is free; when the "usb" thread holds it, wake
// that thread, which runs dap_edpt_service() after tud_task().
static void dap_kick_usb(void)
{
	if (os_mutex_try_lock(&usb_mutex) == os_ok) {
		dap_edpt_service();
		os_mutex_unlock(&usb_mutex);
	} else {
		os_semaphore_post(&usb_sem);
	}
}

void *dap_thread(void *ptr)
{
	uint32_t n;
	do
	{
		while(USBRequestBuffer.rptr != USBRequestBuffer.wptr)
		{
			// The request's data, after its index.
			__dmb();

			/*
			 * Atomic command support - buffer QueueCommands, but don't process them
			 * until a non-QueueCommands packet is seen.
			 */
			n = USBRequestBuffer.rptr;
			while (USBRequestBuffer.data[n % DAP_PACKET_COUNT][0] == ID_DAP_QueueCommands) {
				probe_info("%u %u DAP queued cmd %s len %02x\n",
					       USBRequestBuffer.wptr, USBRequestBuffer.rptr,
					       dap_cmd_string[USBRequestBuffer.data[n % DAP_PACKET_COUNT][0]], USBRequestBuffer.data[n % DAP_PACKET_COUNT][1]);
				USBRequestBuffer.data[n % DAP_PACKET_COUNT][0] = ID_DAP_ExecuteCommands;
				n++;
				while (n == USBRequestBuffer.wptr) {
					/* Wait in a loop here, as IN callbacks will also wake the thread */
					probe_info("DAP wait\n");
					dap_wait_change(&USBRequestBuffer.wptr, n);
				}
				__dmb();
			}
			uint32_t t_start = time_us_32();
			dap_stats.queue_us += t_start - t_out[RD_IDX(USBRequestBuffer)];

			// Read a single packet from the USB buffer into the DAP Request buffer
			memcpy(DAPRequestBuffer, RD_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);
			probe_info("%u %u DAP cmd %s len %02x\n",
				       USBRequestBuffer.wptr, USBRequestBuffer.rptr,
				       dap_cmd_string[DAPRequestBuffer[0]], DAPRequestBuffer[1]);

			// Free the slot; if the OUT endpoint was stalled on it, re-arm
			// it before running the command, so the host can send the next.
			__dmb();
			USBRequestBuffer.rptr++;
			if(out_stalled)
			{
				dap_kick_usb();
			}

			uint32_t t_exec = time_us_32();
			_resp_len = DAP_ExecuteCommand(DAPRequestBuffer, DAPResponseBuffer);
			uint32_t t_done = time_us_32();
			uint32_t exec = t_done - t_exec;
			dap_stats.exec_us += exec;
			if (exec > dap_stats.exec_max_us)
				dap_stats.exec_max_us = exec;
			dap_stats.packets++;
			probe_info("%u %u DAP resp %s\n",
					USBResponseBuffer.wptr, USBResponseBuffer.rptr,
					dap_cmd_string[DAPResponseBuffer[0]]);

			// Wait for a free response slot (the IN callback frees them).
			while(buffer_full(&USBResponseBuffer))
			{
				dap_wait_change(&USBResponseBuffer.rptr, USBResponseBuffer.rptr);
			}
			t_resp[WR_IDX(USBResponseBuffer)] = time_us_32();
			dap_stats.slot_wait_us += t_resp[WR_IDX(USBResponseBuffer)] - t_done;
			t_last_activity = t_resp[WR_IDX(USBResponseBuffer)];

			memcpy(WR_SLOT_PTR(USBResponseBuffer), DAPResponseBuffer, (uint16_t) _resp_len);
			USBResponseBuffer.resp_len[WR_IDX(USBResponseBuffer)] = (uint16_t) _resp_len;

			// Publish the response, its data before its index, and send it.
			__dmb();
			USBResponseBuffer.wptr++;
			dap_kick_usb();
		}

		// Wait for the next request; a USB callback wakes a sleeping thread.
		dap_wait_change(&USBRequestBuffer.wptr, USBRequestBuffer.rptr);

	} while (1);

	return NULL;
}

usbd_class_driver_t const _dap_edpt_driver =
{
		.init = dap_edpt_init,
		.reset = dap_edpt_reset,
		.open = dap_edpt_open,
		.control_xfer_cb = dap_edpt_control_xfer_cb,
		.xfer_cb = dap_edpt_xfer_cb,
		.sof = NULL,
#if CFG_TUSB_DEBUG >= 2
		.name = "DAP ENDPOINT"
#endif
};

// Add the custom driver to the tinyUSB stack
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = 1;
	return &_dap_edpt_driver;
}

