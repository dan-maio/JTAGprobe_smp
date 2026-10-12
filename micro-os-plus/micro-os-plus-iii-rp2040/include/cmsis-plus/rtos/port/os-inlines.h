/*
 * os-inlines.h -- inline port implementations for the RP2040 (2x Cortex-M0+)
 * SMP port of the shared micro-os-plus-iii kernel.
 *
 * Derived from the RP2350 port's (micro-os-plus-iii-cortexm,
 * include-rp2350/cmsis-plus/rtos/port/os-inlines.h), SMP branch only:
 *
 *  - per-core lock_state, and the recursive kernel spinlock `_smp_klock`
 *    backed by the SIO hardware spinlock PICO_SPINLOCK_ID_OS1 (14), the one
 *    the Pico SDK reserves for an operating system;
 *  - PRIMASK critical sections (ARMv6-M has no BASEPRI);
 *  - the high-resolution clock is the 1 MHz system timer the Pico SDK
 *    already runs (time_us_64()).
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_
#define CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_

// ----------------------------------------------------------------------------

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/os-c-decls.h>

// ----------------------------------------------------------------------------

#ifdef __cplusplus

#if !defined(OS_USE_SMP_SCHEDULER)
#error "The RP2040 port is SMP only: define OS_USE_SMP_SCHEDULER and OS_NCPU=2."
#endif

#if !defined(__ARM_ARCH_6M__)
#error "The RP2040 port is for the Cortex-M0+ (ARMv6-M)."
#endif

#include <cmsis_device.h>

#include <hardware/structs/sio.h>
#include <hardware/sync.h>
#include <hardware/timer.h>

#include <cmsis-plus/diag/trace.h>

namespace os
{
  namespace rtos
  {
    namespace port
    {
      // ----------------------------------------------------------------------

      namespace scheduler
      {

        inline void __attribute__ ((always_inline))
        greeting (void)
        {
          trace::printf (
              "Scheduler: µOS++ RP2040 Cortex-M0+ SMP, 2 cores, DI/EI\n");
          trace::printf ("System clock: %u Hz\n", SystemCoreClock);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        lock (void)
        {
          return locked (state::locked);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        unlock (void)
        {
          return locked (state::unlocked);
        }

        // Current core index, read from SIO_CPUID.
        inline unsigned __attribute__ ((always_inline))
        port_cpu_id (void)
        {
          return sio_hw->cpuid;
        }

        inline bool __attribute__ ((always_inline))
        locked (void)
        {
          return lock_state[port_cpu_id ()] != state::unlocked;
        }

        inline void __attribute__ ((always_inline))
        wait_for_interrupt (void)
        {
#if !defined(OS_EXCLUDE_RTOS_IDLE_SLEEP)
          // The DSB is recommended by ARM before WFI.
          __DSB ();
          __WFI ();
#endif /* !defined(OS_EXCLUDE_RTOS_IDLE_SLEEP) */
        }

        // ---- SMP kernel lock (SIO hardware spinlock OS1) --------------------

        // Claim the hardware spinlock (reading it returns 0 while the other
        // core holds it; any write releases it).
        inline void __attribute__ ((always_inline))
        _smp_klock_raw_acquire (void)
        {
          while (sio_hw->spinlock[PICO_SPINLOCK_ID_OS1] == 0)
            {
            }
          __asm__ volatile ("dmb" ::: "memory");
        }

        inline void __attribute__ ((always_inline))
        _smp_klock_raw_release (void)
        {
          __asm__ volatile ("dmb" ::: "memory");
          sio_hw->spinlock[PICO_SPINLOCK_ID_OS1] = 1;
        }

        // Mask IRQs on this core and enter the kernel lock (recursive, with
        // owner tracking so nested critical sections only bump the depth).
        inline void __attribute__ ((always_inline))
        port_set_lock (void)
        {
          __asm__ volatile ("cpsid i" ::: "memory");
          unsigned cpu = port_cpu_id ();
          if (_smp_klock.owner != cpu)
            {
              _smp_klock_raw_acquire ();
              _smp_klock.owner = cpu;
            }
          _smp_klock.depth = _smp_klock.depth + 1;
        }

        // Leave the kernel lock and restore the given PRIMASK state.
        inline void __attribute__ ((always_inline))
        port_put_lock (uint32_t pri)
        {
          unsigned cpu = port_cpu_id ();
          if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
            {
              uint32_t d = _smp_klock.depth - 1;
              _smp_klock.depth = d;
              if (d == 0)
                {
                  _smp_klock.owner = SMP_NO_OWNER;
                  _smp_klock_raw_release ();
                }
            }
          __set_PRIMASK (pri);
        }

      } /* namespace scheduler */

      namespace interrupts
      {
        inline bool __attribute__ ((always_inline))
        in_handler_mode (void)
        {
          // In Handler mode, IPSR holds the exception number.
          // If 0, the core is in thread mode.
          return (__get_IPSR () != 0);
        }

        inline bool
        is_priority_valid (void)
        {
          // With DI/EI (PRIMASK) critical sections, all priorities are valid.
          return true;
        }

        // Enter an IRQ critical section: mask this core's IRQs and take the
        // kernel spinlock so the shared data is also safe against the other
        // core. Recursive (owner+depth), so nested sections are fine.
        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        critical_section::enter (void)
        {
          uint32_t pri = __get_PRIMASK ();
          scheduler::port_set_lock ();
          return pri;
        }

        // Exit an IRQ critical section.
        inline void __attribute__ ((always_inline))
        critical_section::exit (rtos::interrupts::state_t state)
        {
          scheduler::port_put_lock (state);
        }

        // Enter an IRQ uncritical section.
        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        uncritical_section::enter (void)
        {
          uint32_t pri = __get_PRIMASK ();

          // Enable all interrupts.
          __enable_irq ();

          return pri;
        }

        // Exit an IRQ uncritical section.
        inline void __attribute__ ((always_inline))
        uncritical_section::exit (rtos::interrupts::state_t state)
        {
          // Restore PRIMASK to the value saved by enter().
          __set_PRIMASK (state);
        }

      } /* namespace interrupts */

      // ======================================================================

      namespace this_thread
      {
        inline void __attribute__ ((always_inline))
        prepare_suspend (void)
        {
          ;
        }

      } /* namespace this_thread */

      // ======================================================================

      // The Pico SDK runtime has already started the 1 MHz system timer.
      inline void __attribute__ ((always_inline))
      clock_highres::start (void)
      {
      }

      inline constexpr bool __attribute__ ((always_inline))
      clock_highres::has_hardware_counter (void) noexcept
      {
        return true;
      }

      inline uint64_t __attribute__ ((always_inline))
      clock_highres::hardware_counter (void) noexcept
      {
        return time_us_64 ();
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::input_clock_frequency_hz (void)
      {
        return 1000000U;
      }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_per_tick (void)
      {
        return SysTick->LOAD + 1;
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_since_tick (void)
      {
        uint32_t load_value = SysTick->LOAD;

        // Initial sample of the decrementing counter.
        uint32_t val = SysTick->VAL;

        // Check overflow. If the exception is pending, the total cycles
        // count in steady_count_ does not yet reflect the correct value.
        if ((SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) != 0)
          {
            uint32_t val_subsequent = SysTick->VAL;

            // If the value did decrease, the timer did not recycle
            // between the two reads.
            if (val > val_subsequent)
              {
                return load_value + 1 + (load_value - val);
              }
          }

        return load_value - val;
      }
#pragma GCC diagnostic pop

      // ======================================================================

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

// ----------------------------------------------------------------------------

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_ */
