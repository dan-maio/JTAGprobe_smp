/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <pico/stdlib.h>
#include <hardware/uart.h>

#include "tusb.h"

#include "probe.h"
#include "probe_os.h"
#include "board_pico_config.h"
#include "cdc_uart.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* The µOS++ system clock ticks at OS_INTEGER_SYSTICK_FREQUENCY_HZ (1 kHz). */
#define TICK_RATE_HZ OS_INTEGER_SYSTICK_FREQUENCY_HZ

os_clock_timestamp_t last_wake;
os_clock_duration_t interval = 100;
volatile os_clock_timestamp_t break_expiry;
volatile bool timed_break;

/*
 * The CDC callbacks run in the "usb" thread, inside tud_task(); the UART
 * belongs to the "uart" thread. A callback therefore neither touches the UART
 * nor parks the thread: it leaves a request here and posts uart_sem, and the
 * "uart" thread carries it out. The requests are written and read with
 * usb_mutex held.
 */
static volatile bool uart_active = true;       /* polling (DTR or RTS set) */
static bool coding_pending;
static cdc_line_coding_t pending_coding;
static bool break_pending;
static uint16_t pending_break;

/* Max 1 FIFO worth of data */
static uint8_t tx_buf[32];
static uint8_t rx_buf[32];
// Actually s^-1 so 25ms
#define DEBOUNCE_MS 40
static uint debounce_ticks = 5;

void cdc_uart_init(void) {
    gpio_set_function(PROBE_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PROBE_UART_RX, GPIO_FUNC_UART);
    gpio_set_pulls(PROBE_UART_TX, 1, 0);
    gpio_set_pulls(PROBE_UART_RX, 1, 0);
    uart_init(PROBE_UART_INTERFACE, PROBE_UART_BAUDRATE);

#ifdef PROBE_UART_HWFC
    gpio_set_pulls(PROBE_UART_CTS, 0, 1);
    gpio_set_function(PROBE_UART_RTS, GPIO_FUNC_UART);
    gpio_set_function(PROBE_UART_CTS, GPIO_FUNC_UART);
    uart_set_hw_flow(PROBE_UART_INTERFACE, true, true);
#else
#ifdef PROBE_UART_RTS
    gpio_init(PROBE_UART_RTS);
    gpio_set_dir(PROBE_UART_RTS, GPIO_OUT);
    gpio_put(PROBE_UART_RTS, 1);
#endif
#endif

#ifdef PROBE_UART_DTR
    gpio_init(PROBE_UART_DTR);
    gpio_set_dir(PROBE_UART_DTR, GPIO_OUT);
    gpio_put(PROBE_UART_DTR, 1);
#endif
}

/* Apply a new line coding ("uart" thread). */
static void cdc_apply_line_coding(cdc_line_coding_t const* line_coding)
{
    uart_parity_t parity;
    uint data_bits, stop_bits;
    /* Set the tick thread interval to the amount of time it takes to
     * fill up half a FIFO. Millis is too coarse for integer divide.
     */
    uint32_t micros = (1000 * 1000 * 16 * 10) / MAX(line_coding->bit_rate, 1);
    interval = MAX(1, micros / ((1000 * 1000) / TICK_RATE_HZ));
    debounce_ticks = MAX(1, TICK_RATE_HZ / (interval * DEBOUNCE_MS));
    probe_info("New baud rate %ld micros %ld interval %lu\n",
               line_coding->bit_rate, micros, interval);
    uart_deinit(PROBE_UART_INTERFACE);
    os_mutex_lock(&usb_mutex);
    tud_cdc_write_clear();
    tud_cdc_read_flush();
    os_mutex_unlock(&usb_mutex);
    uart_init(PROBE_UART_INTERFACE, line_coding->bit_rate);

    switch (line_coding->parity) {
    case CDC_LINE_CODING_PARITY_ODD:
        parity = UART_PARITY_ODD;
        break;
    case CDC_LINE_CODING_PARITY_EVEN:
        parity = UART_PARITY_EVEN;
        break;
    default:
        probe_info("invalid parity setting %u\n", line_coding->parity);
        /* fallthrough */
    case CDC_LINE_CODING_PARITY_NONE:
        parity = UART_PARITY_NONE;
        break;
    }

    switch (line_coding->data_bits) {
    case 5:
    case 6:
    case 7:
    case 8:
        data_bits = line_coding->data_bits;
        break;
    default:
        probe_info("invalid data bits setting: %u\n", line_coding->data_bits);
        data_bits = 8;
        break;
    }

    /* The PL011 only supports 1 or 2 stop bits. 1.5 stop bits is translated to 2 */
    switch (line_coding->stop_bits) {
    case CDC_LINE_CODING_STOP_BITS_1_5:
    case CDC_LINE_CODING_STOP_BITS_2:
        stop_bits = 2;
        break;
    default:
        probe_info("invalid stop bits setting: %u\n", line_coding->stop_bits);
        /* fallthrough */
    case CDC_LINE_CODING_STOP_BITS_1:
        stop_bits = 1;
        break;
    }

    uart_set_format(PROBE_UART_INTERFACE, data_bits, stop_bits, parity);
}

/* Apply a break request ("uart" thread). */
static void cdc_apply_break(uint16_t wValue)
{
    switch (wValue) {
    case 0:
        uart_set_break(PROBE_UART_INTERFACE, false);
        timed_break = false;
        break;
    case 0xffff:
        uart_set_break(PROBE_UART_INTERFACE, true);
        timed_break = false;
        break;
    default:
        uart_set_break(PROBE_UART_INTERFACE, true);
        timed_break = true;
        break_expiry = os_sysclock_now() + (wValue * (TICK_RATE_HZ / 1000));
        break;
    }
}

/* Carry out what the CDC callbacks asked for ("uart" thread). */
static void cdc_apply_requests(void)
{
    cdc_line_coding_t coding;
    bool do_coding, do_break;
    uint16_t brk;

    os_mutex_lock(&usb_mutex);
    do_coding = coding_pending;
    coding = pending_coding;
    coding_pending = false;
    do_break = break_pending;
    brk = pending_break;
    break_pending = false;
    os_mutex_unlock(&usb_mutex);

    if (do_coding) {
        cdc_apply_line_coding(&coding);
    }
    if (do_break) {
        cdc_apply_break(brk);
    }
}

bool cdc_task(void)
{
    static int was_connected = 0;
    uint rx_len = 0;
    size_t tx_len = 0;
    bool connected;
    bool disconnected = false;
    bool keep_alive = false;

    // Consume uart fifo regardless even if not connected
    while (uart_is_readable(PROBE_UART_INTERFACE) && (rx_len < sizeof(rx_buf))) {
        rx_buf[rx_len++] = uart_getc(PROBE_UART_INTERFACE);
    }

    // tinyusb, with usb_mutex held; the UART itself outside it.
    os_mutex_lock(&usb_mutex);
    connected = tud_cdc_connected();
    if (connected) {
        was_connected = 1;
        int written = 0;
        if (rx_len) {
            written = MIN(tud_cdc_write_available(), rx_len);
            if (written > 0) {
                tud_cdc_write(rx_buf, written);
                tud_cdc_write_flush();
            }
        }

        /* Reading from USB, to be written to the UART FIFO. */
        size_t watermark = MIN(tud_cdc_available(), sizeof(tx_buf));
        if (watermark > 0) {
            watermark = MIN(watermark, 16);
            tx_len = tud_cdc_read(tx_buf, watermark);
        }
    } else if (was_connected) {
        tud_cdc_write_clear();
        was_connected = 0;
        disconnected = true;
    }
    os_mutex_unlock(&usb_mutex);

    if (connected) {
        if (tx_len > 0) {
            uart_write_blocking(PROBE_UART_INTERFACE, tx_buf, tx_len);
        }

        /* Pending break handling */
        if (timed_break) {
            if ((int64_t)(break_expiry - os_sysclock_now()) < 0) {
                timed_break = false;
                uart_set_break(PROBE_UART_INTERFACE, false);
            } else {
                keep_alive = true;
            }
        }
    } else if (disconnected) {
        uart_set_break(PROBE_UART_INTERFACE, false);
        timed_break = false;
    }
    return keep_alive;
}

void *cdc_thread(void *ptr)
{
    bool keep_alive;
    last_wake = os_sysclock_now();
    /* Threaded with a polling interval that scales according to linerate */
    while (1) {
        cdc_apply_requests();
        if (!uart_active) {
            /* Interface deactivated: sleep until a CDC callback posts. */
            os_semaphore_wait(&uart_sem);
            last_wake = os_sysclock_now();
            continue;
        }
        keep_alive = cdc_task();
        if (!keep_alive) {
            /* Periodic wake-up, as xTaskDelayUntil(); restart the period
             * if it was already missed. */
            last_wake += interval;
            os_clock_timestamp_t now = os_sysclock_now();
            if (last_wake > now)
                os_semaphore_timed_wait(&uart_sem, (os_clock_duration_t)(last_wake - now));
            else
                last_wake = now;
        }
    }
    return NULL;
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* line_coding)
{
    /* "usb" thread, usb_mutex held: hand it to the "uart" thread. */
    pending_coding = *line_coding;
    coding_pending = true;
    os_semaphore_post(&uart_sem);
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
#ifdef PROBE_UART_RTS
    gpio_put(PROBE_UART_RTS, !rts);
#endif
#ifdef PROBE_UART_DTR
    gpio_put(PROBE_UART_DTR, !dtr);
#endif

    /* CDC drivers use linestate as a bodge to activate/deactivate the interface.
     * Resume our UART polling on activate, stop on deactivate */
    uart_active = (dtr || rts);
    os_semaphore_post(&uart_sem);
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t wValue) {
    /* "usb" thread, usb_mutex held: hand it to the "uart" thread. */
    pending_break = wValue;
    break_pending = true;
    os_semaphore_post(&uart_sem);
}
