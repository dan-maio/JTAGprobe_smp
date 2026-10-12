/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) 2016-2025 Liviu Ionescu. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * If a copy of the license was not distributed with this file, it can
 * be obtained from https://opensource.org/licenses/mit/.
 */

/*
 * os-decls.h -- the RP2040 (2x Cortex-M0+) SMP port's C++ declarations.
 *
 * The Cortex-M port's (micro-os-plus-iii-cortexm,
 * include/cmsis-plus/rtos/port/os-decls.h), which the RP2350 port also uses.
 * Included by `cmsis-plus/rtos/os.h`.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_

// ----------------------------------------------------------------------------

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/port/os-c-decls.h>

#if !defined(OS_INTEGER_SYSTICK_FREQUENCY_HZ)
#define OS_INTEGER_SYSTICK_FREQUENCY_HZ (1000)
#endif

#if !defined(OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES (256)
#endif

#if !defined(OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES (2048)
#endif

#if !defined(OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES \
  (OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#endif

#if !defined(OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES \
  (OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#endif

// ----------------------------------------------------------------------------

#include <signal.h>
// Platform definitions
#include <sys/time.h>

// ----------------------------------------------------------------------------

#ifdef __cplusplus

#include <cstdint>
#include <cstddef>

namespace os
{
  namespace rtos
  {
    namespace port
    {
      // ----------------------------------------------------------------------

      namespace stack
      {
        // Stack word.
        using element_t = os_port_thread_stack_element_t;

        // Align stack to 8 bytes.
        using allocation_element_t = os_port_thread_stack_allocation_element_t;

        // Initial value for the minimum stack size in bytes.
        constexpr std::size_t min_size_bytes
            = OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES;

        // Initial value for the default stack size in bytes.
        constexpr std::size_t default_size_bytes
            = OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES;

        // Used to fill in the stack.
        constexpr element_t magic
            = OS_INTEGER_RTOS_STACK_FILL_MAGIC; // DEADBEEF

      } /* namespace stack */

      namespace interrupts
      {
        // Type to store the entire processor interrupts mask.
        using state_t = os_port_irq_state_t;

        namespace state
        {
          constexpr state_t init = 0;
        } /* namespace state */

      } /* namespace interrupts */

      namespace scheduler
      {
        using state_t = os_port_scheduler_state_t;

        namespace state
        {
          constexpr state_t locked = true;
          constexpr state_t unlocked = false;
          constexpr state_t init = unlocked;
        } /* namespace state */

#if defined(OS_USE_SMP_SCHEDULER)

        // One lock state per CPU, as the SMP kernel indexes it, and the
        // PRIMASK each CPU had when it took the scheduler lock.
        extern state_t lock_state[OS_NCPU];
        extern uint32_t lock_primask[OS_NCPU];

        // The kernel lock, recursive. owner/depth record who holds it and how
        // deeply; what excludes the other core is the SIO hardware spinlock
        // PICO_SPINLOCK_ID_OS1 (os-inlines.h).
        struct smp_klock_t
        {
          volatile uint32_t owner;
          volatile uint32_t depth;
        };

        extern smp_klock_t _smp_klock;

#else

        extern state_t lock_state;

#endif /* defined(OS_USE_SMP_SCHEDULER) */

      } /* namespace scheduler */

      using thread_context_t = struct context_s
      {
        // On Cortex-M cores the context itself is stored on the stack,
        // only the stack pointer needs to be preserved.
        stack::element_t* stack_ptr;
      };

      // ----------------------------------------------------------------------

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

// ----------------------------------------------------------------------------

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_ */
