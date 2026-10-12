/*
 * os-app-config.h -- µOS++ configuration for JTAGprobe_smp (RP2040, 2x
 * Cortex-M0+). Read by the kernel and the port under OS_USE_OS_APP_CONFIG_H.
 */

#ifndef CMSIS_PLUS_OS_APP_CONFIG_H_
#define CMSIS_PLUS_OS_APP_CONFIG_H_

// SysTick RTOS tick: 1000 Hz -> 1 tick == 1 ms.
#define OS_INTEGER_SYSTICK_FREQUENCY_HZ (1000)

// The "main" thread runs os_main(): the probe's initialisation, then it
// waits on the "usb" thread.
#define OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES (2048)

// The core-0 idle thread (the core-1 one is the port's "idle1").
#define OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES (1024)

#define OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES (2048)

#endif /* CMSIS_PLUS_OS_APP_CONFIG_H_ */
