#ifndef CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/port/os-c-decls.h>

#if !defined(OS_INTEGER_SYSTICK_FREQUENCY_HZ)
#define OS_INTEGER_SYSTICK_FREQUENCY_HZ (1000)
#endif

#if !defined(OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES (2048)
#endif

#if !defined(OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES (16384)
#endif

#if !defined(OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES \
  (OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#endif

#if !defined(OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES)
#define OS_INTEGER_RTOS_IDLE_STACK_SIZE_BYTES \
  (OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES)
#endif

#include <signal.h>
#include <sys/time.h>

#ifdef __cplusplus

#include <cstdint>
#include <cstddef>

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace stack
      {
        using element_t = os_port_thread_stack_element_t;
        using allocation_element_t = os_port_thread_stack_allocation_element_t;

        constexpr std::size_t min_size_bytes = OS_INTEGER_RTOS_MIN_STACK_SIZE_BYTES;
        constexpr std::size_t default_size_bytes = OS_INTEGER_RTOS_DEFAULT_STACK_SIZE_BYTES;
        constexpr element_t magic = OS_INTEGER_RTOS_STACK_FILL_MAGIC;
      }

      namespace interrupts
      {
        using state_t = os_port_irq_state_t;

        namespace state
        {
          constexpr state_t init = 0;
        }
      }

      namespace scheduler
      {
        using state_t = os_port_scheduler_state_t;

        namespace state
        {
          constexpr state_t locked = true;
          constexpr state_t unlocked = false;
          constexpr state_t init = unlocked;
        }

        extern volatile state_t lock_state[OS_NCPU];

        struct smp_klock_t
        {
          volatile uint32_t lock;
          volatile uint32_t owner;
          volatile uint32_t depth;
        };

        struct smp_tlock_t
        {
          volatile uint32_t lock;
        };

        extern smp_klock_t _smp_klock;
        extern smp_tlock_t _smp_tlock;
        extern volatile unsigned _port_ctx_pending[OS_NCPU];
      }

      using thread_context_t = struct context_s
      {
        stack::element_t* stack_ptr;
      };

    }
  }
}

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_DECLS_H_ */
