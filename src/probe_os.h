/*
 * probe_os.h -- the µOS++ objects the probe's threads share.
 *
 * Who runs where (RP2040, 2x Cortex-M0+, µOS++ III SMP):
 *
 *   core 0   "usb"   tud_task() and every other tinyusb call, plus the
 *                    DAP endpoint transfers (tusb_edpt_handler.c)
 *            "uart"  the USB CDC <-> UART bridge (cdc_uart.c)
 *   core 1   "dap"   DAP_ExecuteCommand(): the JTAG/SWD bit-banging, which
 *                    also carries the target's semihosting traffic
 *
 * tinyusb is not thread-safe and is configured without an OS (OPT_OS_NONE),
 * so every call into it is made with usb_mutex held, and only from core 0.
 * The DAP thread never calls tinyusb: it takes requests from, and puts
 * responses into, the two rings in tusb_edpt_handler.c, and wakes the USB
 * thread with usb_sem.
 */

#ifndef PROBE_OS_H_
#define PROBE_OS_H_

#include <cmsis-plus/rtos/os-c-api.h>

/* Serialises tinyusb between the "usb" and "uart" threads (both core 0). */
extern os_mutex_t usb_mutex;

/* Wakes the "usb" thread: posted by the USB IRQ (tud_event_hook_cb) and by
 * the "dap" thread when it has a response or freed a request slot. */
extern os_semaphore_t usb_sem;

/* Wakes the "dap" thread: posted by the endpoint callbacks when a request
 * arrived or a response slot was freed. */
extern os_semaphore_t dap_sem;

/* Wakes the "uart" thread: posted by the CDC callbacks. */
extern os_semaphore_t uart_sem;

#endif /* PROBE_OS_H_ */
