/*
 * os-core-rp2040.cpp -- the RP2040 (2x Cortex-M0+) SMP port of the shared
 * micro-os-plus-iii kernel, for an application booted by the Pico SDK.
 *
 * Derived from the RP2350 port (micro-os-plus-iii-cortexm,
 * src/rtos/os-core-rp2350.cpp), SMP branch, with the ARMv6-M context switch of
 * upstream's Cortex-M core (micro-os-plus-iii-cortexm, src/rtos/os-core.cpp).
 *
 *  - Context switch: cooperative PendSV (lowest priority), one per core. The
 *    PendSV handler runs on the MSP, so the outgoing thread's frame on its PSP
 *    is quiescent once save_on_stack() returns, and switch_stacks() publishes
 *    its stack_ptr at once.
 *  - Per-core SysTick: only core 0 advances the RTOS clock
 *    (os_systick_handler()); core 1's SysTick only reschedules core 1.
 *  - Cross-core IPI: the SIO inter-core FIFO. The RP2040 gives each core its
 *    own IRQ for it, SIO_IRQ_PROC0 (15) and SIO_IRQ_PROC1 (16); the handler
 *    drains the FIFO and pends its own core's PendSV.
 *  - Mutual exclusion: the recursive kernel spinlock `_smp_klock`, backed by
 *    the SIO hardware spinlock PICO_SPINLOCK_ID_OS1 (os-inlines.h).
 *  - Boot: the Pico SDK's crt0 and runtime run first and call the kernel's
 *    main(); its vector table is the SDK's, with the CMSIS names
 *    (cmsis_core), so the handlers below replace the SDK's weak ones. Core 1
 *    is launched by os_rp2040_start_core1() on the SDK's core-1 stack.
 */

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>

#include <cmsis_device.h>

#include <hardware/structs/sio.h>
#include <hardware/structs/scb.h>
#include <pico/multicore.h>

#include <micro-os-plus-rp2040.h>

#include <cstring>

extern "C"
{
  // Pico SDK linker script: the core-0 MSP stack (SCRATCH_Y) and the core-1
  // stack (SCRATCH_X).
  extern unsigned int __StackBottom;
  extern unsigned int __StackTop;
  extern unsigned int __StackOneTop;

  void os_systick_handler (void); // kernel: src/rtos/os-clocks.cpp

  // Pico SDK linker script: the heap, __end__ .. __HeapLimit.
  extern char __end__;
  extern char __HeapLimit;

  void
  os_startup_initialize_free_store (void* heap_address,
                                    unsigned int heap_size_bytes);
}

// ----------------------------------------------------------------------------

// What the kernel's own startup (micro-os-plus::iii-startup) would do, and
// the Pico SDK's runtime does not: the pico2 board's glue does the same
// (micro-os-plus-iii-cortexm, test/boards/pico2/glue/rtos-glue.cpp and
// src/init-fini-stubs.c).

// Give the RTOS free store the SDK's heap, before any C++ constructor and
// the kernel's main() allocate. Runs from the SDK runtime's
// __libc_init_array(), ahead of main().
extern "C" __attribute__ ((constructor (101))) void
os_rp2040_initialize_free_store (void)
{
  os_startup_initialize_free_store (
      &__end__, static_cast<unsigned int> (&__HeapLimit - &__end__));
}

// exit() calls it; the probe's threads never end, so there is nothing to
// run.
extern "C" void
os_run_fini_array (void)
{
}

// The kernel's idle thread (core 0) and the per-core idle array
// (os-idle.cpp / os-core.cpp); neither is declared in a public header.
extern os::rtos::thread* os_idle_thread;
namespace os
{
  namespace rtos
  {
    namespace scheduler
    {
      extern thread* os_idle_thread_core[OS_NCPU];
    }
  }
}

namespace
{
  // Lowest exception priority on the M0+ (__NVIC_PRIO_BITS == 2).
  constexpr uint32_t lowest_priority = (1UL << __NVIC_PRIO_BITS) - 1UL;

  // Drain this core's RX FIFO and clear the sticky WOF/ROE flags.
  inline void
  fifo_drain (void)
  {
    while (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)
      {
        volatile uint32_t d = sio_hw->fifo_rd;
        (void) d;
      }
    sio_hw->fifo_st = SIO_FIFO_ST_WOF_BITS | SIO_FIFO_ST_ROE_BITS;
  }
} // namespace

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace stack
      {
        // Stack frame, as used by PendSV (see upstream's Cortex-M core for
        // the full layout documentation).

        typedef struct frame_s
        {
          // Saved by save_on_stack(): r4-r7, then r8-r11 and EXC_RETURN.
          stack::element_t r4;
          stack::element_t r5;
          stack::element_t r6;
          stack::element_t r7;
          stack::element_t r8;
          stack::element_t r9;
          stack::element_t r10_sl;
          stack::element_t r11_fp;
          stack::element_t r14_exec_return;
          // Restored automatically by exception return.
          stack::element_t r0;
          stack::element_t r1;
          stack::element_t r2;
          stack::element_t r3;
          stack::element_t r12;
          stack::element_t r14_lr;
          stack::element_t r15_pc;
          stack::element_t psr;
        } frame_t;
      } /* namespace stack */

      /**
       * @brief Create a new thread context on the stack.
       */
      void
      context::create (void* context, void* func, void* args)
      {
        class rtos::thread::context* th_ctx
            = static_cast<class rtos::thread::context*> (context);

        class rtos::thread::stack& stack = th_ctx->stack ();

        rtos::thread::stack::element_t* p = stack.top ();

        // Be sure the stack is large enough to hold at least
        // two exception frames.
        assert ((p - stack.bottom ())
                > static_cast<int> (2 * sizeof (stack::frame_t)));

        p -= (sizeof (stack::frame_t)
              / sizeof (rtos::thread::stack::element_t));

        // Align the frame to 8 bytes than leave one more word for the extra
        // stack element, r14_exec_return, which is the 9th.
        if ((reinterpret_cast<uintptr_t> (p) & 3) != 0)
          {
            p = reinterpret_cast<rtos::thread::stack::element_t*> (
                (reinterpret_cast<int> (p)) & (~3));
          }

        if ((reinterpret_cast<uintptr_t> (p) & 7) == 0)
          {
            --p;
          }

        stack::frame_t* f = reinterpret_cast<stack::frame_t*> (p);

        f->psr = 0x01000000; // xPSR, T bit set
        f->r15_pc = static_cast<rtos::thread::stack::element_t> (
            (reinterpret_cast<ptrdiff_t> (func)) | 1);
        f->r14_lr = static_cast<rtos::thread::stack::element_t> (
            (reinterpret_cast<ptrdiff_t> (func) + 2));
        f->r12 = 0xCCCCCCCC + 0x00010203;
        f->r3 = 0x33333333 + 0x00010203;
        f->r2 = 0x22222222 + 0x00010203;
        f->r1 = 0x11111111 + 0x00010203;
        f->r0 = reinterpret_cast<rtos::thread::stack::element_t> (args);
        f->r14_exec_return = 0xFFFFFFFD; // return to thread, PSP, 8-word frame
        f->r11_fp = 0xBBBBBBBB + 0x00010203;
        f->r10_sl = 0xAAAAAAAA + 0x00010203;
        f->r9 = 0x99999999 + 0x00010203;
        f->r8 = 0x88888888 + 0x00010203;
        f->r7 = 0x77777777 + 0x00010203;
        f->r6 = 0x66666666 + 0x00010203;
        f->r5 = 0x55555555 + 0x00010203;
        f->r4 = 0x44444444 + 0x00010203;

        th_ctx->port_.stack_ptr = p;

        assert (((reinterpret_cast<int> (&f->r0)) & 7) == 0);
      }

      void
      clock_systick::start (void)
      {
        // The Pico SDK's runtime set clk_sys; read it back for SysTick.
        SystemCoreClockUpdate ();
#if defined(NDEBUG)
        SysTick_Config (SystemCoreClock / rtos::clock_systick::frequency_hz);
#else
        assert (SysTick_Config (SystemCoreClock
                                / rtos::clock_systick::frequency_hz)
                == 0);
#endif
        // SysTick at the lowest priority.
        NVIC_SetPriority (SysTick_IRQn, lowest_priority);
      }

      // ----------------------------------------------------------------------

      namespace scheduler
      {
        state_t lock_state[OS_NCPU];
        uint32_t lock_primask[OS_NCPU];
        smp_klock_t _smp_klock = { SMP_NO_OWNER, 0 };

        result_t
        initialize (void)
        {
          __disable_irq ();

          return result::ok;
        }

        /**
         * @brief Start the scheduler on core 0 and pass control to the
         * first thread.
         */
        void
        start (void)
        {
          // Disable all interrupts, to safely change the stack.
          __disable_irq ();

          // Switch the current SP from MSP to PSP without breaking the
          // running code: initialise PSP with the same value as MSP.
          __set_PSP (__get_MSP ());
          __set_CONTROL (__get_CONTROL () | CONTROL_SPSEL_Msk);
          __ISB ();

          // Re-initialise MSP to the reset value. The SDK's vector table
          // (in RAM, VTOR) starts with __StackTop.
          __set_MSP (*(reinterpret_cast<uint32_t*> (SCB->VTOR)));

          // The interrupt stack is the SDK's core-0 stack (SCRATCH_Y). Fill
          // it with the magic word, for the idle thread's stack checks, but
          // only below the words this function is still running on (they
          // are now the PSP's).
          rtos::interrupts::stack ()->set (
              reinterpret_cast<stack::element_t*> (&__StackBottom),
              static_cast<size_t> (&__StackTop - &__StackBottom)
                  * sizeof (__StackTop));
          {
            stack::element_t* p
                = reinterpret_cast<stack::element_t*> (&__StackBottom);
            stack::element_t* end
                = reinterpret_cast<stack::element_t*> (__get_PSP ()) - 16;
            while (p < end)
              {
                *p++ = os::rtos::thread::stack::magic;
              }
          }

          // PendSV at the lowest priority.
          NVIC_SetPriority (PendSV_IRQn, lowest_priority);

          // A fake "current" thread so the first switch has a node to
          // relink (it is discarded immediately, never restored).
          static os_thread_t fake_thread;
          memset (&fake_thread, 0, sizeof (os_thread_t));
          fake_thread.name = "fake_thread";
          rtos::scheduler::current_thread_[0]
              = reinterpret_cast<rtos::thread*> (&fake_thread);

          for (unsigned i = 0; i < OS_NCPU; ++i)
            {
              lock_state[i] = state::init;
            }
          rtos::scheduler::os_idle_thread_core[0] = ::os_idle_thread;

          // Cross-core IPI channel on this core: drain any stale FIFO data.
          // The FIFO IRQ is NOT enabled here: core 1's launch handshake runs
          // over the same FIFO, and the handler would steal its words.
          // os_rp2040_start_core1() enables it after the launch.
          fifo_drain ();
          NVIC_SetPriority (SIO_IRQ_PROC0_IRQn, lowest_priority);

          // Trigger the PendSV; the exception will happen after
          // re-enabling the interrupts.
          scheduler::reschedule ();

          // Enable all interrupts; allow PendSV to occur.
          __enable_irq ();

          // The context switch will occur somewhere here.
          for (;;)
            __NOP ();
        }

        // --------------------------------------------------------------------

        state_t
        locked (state_t state)
        {
          os_assert_throw (!interrupts::in_handler_mode (), EPERM);

          // Mask IRQs BEFORE reading the core id: with them enabled, a PendSV
          // between port_cpu_id() and the lock_state[] write can move this
          // thread to the other core.
          uint32_t pri = __get_PRIMASK ();
          __asm__ volatile ("cpsid i" ::: "memory");
          unsigned cpu = port_cpu_id ();
          state_t tmp = lock_state[cpu];

          if (state != tmp)
            {
              if (state == state::locked)
                {
                  // Take the kernel spinlock; IRQs stay masked.
                  port_set_lock ();
                  lock_primask[cpu] = pri;
                  lock_state[cpu] = state;
                }
              else
                {
                  // Publish "unlocked" BEFORE releasing the spinlock and
                  // restoring PRIMASK: otherwise an IRQ in that window sees
                  // this core still locked and skips the PendSV.
                  lock_state[cpu] = state;
                  port_put_lock (lock_primask[cpu]);
                }
            }
          else
            {
              __set_PRIMASK (pri);
            }
          return tmp;
        }

        // --------------------------------------------------------------------

        /**
         * @brief Request a context switch on the current core.
         *
         * @details
         * PendSV is per-core and lowest priority, so setting it pending on
         * this core defers the switch until the current interrupt (if any)
         * completes.
         */
        void
        reschedule (void)
        {
          if (rtos::scheduler::locked ()
              || (rtos::interrupts::in_handler_mode ()
                  && !rtos::scheduler::preemptive ()))
            {
              return;
            }

          // Set PendSV to request a context switch.
          SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;

          // DSB/ISB recommended by ARM after programming control registers.
          __DSB ();
          __ISB ();
        }

        // --------------------------------------------------------------------

        /**
         * @brief Switch stacks to perform the rescheduling (called from
         * PendSV_Handler, on the MSP).
         *
         * @details
         * The whole switch runs while holding the kernel spinlock, so the
         * other core cannot touch the ready lists concurrently. The incoming
         * thread's context is claimed by zeroing its stack_ptr, matching the
         * kernel SMP pick loop's "stack_ptr != nullptr" guard.
         */
        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          uint32_t pri = __get_PRIMASK ();
          __disable_irq ();

          unsigned cpu = port_cpu_id ();

          // Enter the kernel spinlock (recursive).
          if (_smp_klock.owner != cpu)
            {
              _smp_klock_raw_acquire ();
              _smp_klock.owner = cpu;
            }
          _smp_klock.depth = _smp_klock.depth + 1;

          // Clear the PendSV bit (late-arrival/preemption safety).
          SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk;

          rtos::thread* old_thread = rtos::scheduler::current_thread_[cpu];

          // Publish the outgoing thread's saved frame (safe: this runs on the
          // MSP, the outgoing PSP is no longer in use).
          old_thread->context_.port_.stack_ptr = sp;

          // Pick the next thread (affinity-aware, per CPU).
          rtos::scheduler::internal_switch_threads ();

          rtos::thread* new_thread = rtos::scheduler::current_thread_[cpu];

          if (new_thread == nullptr)
            {
              // No ready thread and no idle thread on this core -- fatal.
              // Release the kernel spinlock so the other core does not
              // deadlock, then halt masked.
              _smp_klock.depth = _smp_klock.depth - 1;
              if (_smp_klock.depth == 0)
                {
                  _smp_klock.owner = SMP_NO_OWNER;
                  _smp_klock_raw_release ();
                }
              __asm__ volatile ("cpsid i" ::: "memory");
              for (;;)
                __asm__ volatile ("wfi");
            }

          stack::element_t* out_sp;
          if (new_thread != old_thread)
            {
              out_sp = new_thread->context_.port_.stack_ptr;
              // Claim: new_thread's context is now live on this CPU; the
              // other core must not pick it.
              new_thread->context_.port_.stack_ptr = nullptr;
            }
          else
            {
              out_sp = sp;
              old_thread->context_.port_.stack_ptr = nullptr;
            }

          // Leave the kernel spinlock.
          _smp_klock.depth = _smp_klock.depth - 1;
          if (_smp_klock.depth == 0)
            {
              _smp_klock.owner = SMP_NO_OWNER;
              _smp_klock_raw_release ();
            }

          __set_PRIMASK (pri);

          return out_sp;
        }

        // --------------------------------------------------------------------

        // ARMv6-M: no stmdb, and stm/ldm reach only r0-r7, so r8-r11 and
        // EXC_RETURN go through r3-r7 (r0-r3 are already in the hardware
        // frame). Upstream's Cortex-M core, the __ARM_ARCH_6M__ branch.
        inline stack::element_t* __attribute__ ((always_inline))
        save_on_stack (void)
        {
          stack::element_t* sp_;

          asm volatile (
              // Get the thread stack
              " mrs %[r], PSP                       \n"
              " isb                                 \n"

              // With decrement not available, manually
              // move the pointer 9 words below.
              " sub %[r], %[r], #36                 \n"

              // Save the core registers r4-r7. Increment [r].
              " stmia %[r]!, {r4-r7}                \n"

              // Move the core registers r8-r11 to lower registers.
              " mov r3, r8                          \n"
              " mov r4, r9                          \n"
              " mov r5, sl                          \n"
              " mov r6, fp                          \n"
              " mov r7, lr                          \n"
              // Save the core registers r8-r11,lr. Increment [r].
              " stmia %[r]!, {r3-r7}                \n"

              // Move [r] down, to cancel increment.
              " sub %[r], %[r], #36                 \n"

              : [r] "=r"(sp_) /* out */
              : /* in */
              : /* clobber. DO NOT add anything here! */
          );

          return sp_;
        }

        inline void __attribute__ ((always_inline))
        restore_from_stack (stack::element_t* sp)
        {
          asm volatile (

              // Move [r] to upper registers location (4*4).
              " add %[r], %[r], #16                 \n"

              // Restore the core registers r8-r11,lr to lower registers.
              // Increment [r].
              " ldmia %[r]!, {r3-r7}                \n"

              // Move lower registers to upper registers r8-r11.
              " mov r8, r3                          \n"
              " mov r9, r4                          \n"
              " mov sl, r5                          \n"
              " mov fp, r6                          \n"
              " mov lr, r7                          \n"

              // Move [r] down, to lower registers location (9*4).
              " sub %[r], %[r], #36                 \n"

              // Restore the core registers r4-r7. Increment [r].
              " ldmia %[r]!, {r4-r7}                \n"

              // Move [r] up, after upper registers (5*4).
              " add %[r], %[r], #20                 \n"

              // Restore the thread stack register.
              " msr PSP, %[r]                       \n"
              " isb                                 \n"

              : /* out */
              : [r] "r"(sp) /* in */
              : /* clobber. DO NOT add anything here! */
          );
        }

      } /* namespace scheduler */
    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

// ----------------------------------------------------------------------------

extern "C"
{
  // Current core index (SIO_CPUID). Provided to the kernel SMP branch.
  unsigned
  port_cpu_id (void)
  {
    return os::rtos::port::scheduler::port_cpu_id ();
  }

  // Cross-core IPI: wake the other core over the SIO inter-core FIFO.
  // Non-blocking: if the FIFO is full the target already has a pending IPI.
  void
  port_smp_ipi (unsigned cpu)
  {
    if (cpu == port_cpu_id ())
      {
        return;
      }
    if (sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS)
      {
        sio_hw->fifo_wr = 0x4D53u; // "SM"
        __SEV ();
      }
  }
}

namespace
{
  // Core-1 entry, from os_rp2040_start_core1() through the SDK's launch
  // handshake, on the SDK's core-1 stack (MSP). Builds a fake thread context
  // for core 1, starts its own SysTick/PendSV/FIFO IRQ, then reschedules
  // into the first core-1 runnable thread (or "idle1").
  void
  port_smp_secondary_start (void)
  {
    using namespace os::rtos;

    __asm__ volatile ("cpsid i" ::: "memory");

    unsigned cpu = port::scheduler::port_cpu_id (); // 1

    // Per-core exceptions: PendSV + SysTick at the lowest priority.
    NVIC_SetPriority (PendSV_IRQn, lowest_priority);
    SysTick_Config (SystemCoreClock / clock_systick::frequency_hz);
    NVIC_SetPriority (SysTick_IRQn, lowest_priority);

    // Cross-core IPI channel: the launch used the FIFO; drain it and enable
    // this core's FIFO IRQ.
    fifo_drain ();
    NVIC_SetPriority (SIO_IRQ_PROC1_IRQn, lowest_priority);
    NVIC_EnableIRQ (SIO_IRQ_PROC1_IRQn);

    // Fake "current" thread for core 1 (never restored). Done BEFORE the
    // PSP switch: no local may be alive across the SPSEL change.
    static os_thread_t fake_thread_sec;
    memset (&fake_thread_sec, 0, sizeof (os_thread_t));
    fake_thread_sec.name = "fake_thread_sec";
    scheduler::current_thread_[cpu]
        = reinterpret_cast<thread*> (&fake_thread_sec);

    // Thread mode onto a PSP, like the core-0 start().
    __set_PSP (__get_MSP ());
    __set_CONTROL (__get_CONTROL () | CONTROL_SPSEL_Msk);
    __ISB ();

    // Trigger the first PendSV; it will fire after __enable_irq().
    port::scheduler::reschedule ();

    __enable_irq ();

    for (;;)
      {
        __asm__ volatile ("wfi");
      }
  }

  // Core 1's idle thread. The kernel recognises it by the reserved name
  // "idle1" and keeps it on core 1 (is_thread_allowed_on_cpu).
  void*
  secondary_idle_func (void*)
  {
    using namespace os::rtos;

    this_thread::thread ().priority (thread::priority::idle);
    while (true)
      {
        __asm__ volatile ("dsb" ::: "memory");
        __asm__ volatile ("wfi");
        this_thread::yield ();
      }
    return nullptr;
  }
} // namespace

void
os_rp2040_start_core1 (void)
{
  using namespace os::rtos;

  // Core 1's idle thread, registered before the launch so core 1's first
  // reschedule always finds a thread.
  static thread::stack::allocation_element_t
      idle1_stack[512 / sizeof (thread::stack::allocation_element_t)];
  static thread* idle1;
  {
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = idle1_stack;
    attr.th_stack_size_bytes = sizeof (idle1_stack);
    idle1 = new thread ("idle1", secondary_idle_func, nullptr, attr);
    scheduler::os_idle_thread_core[1] = idle1;
  }

  // Core 1 runs on the SDK's core-1 stack, with the shared vector table.
  multicore_reset_core1 ();
  multicore_launch_core1_raw (port_smp_secondary_start,
                              reinterpret_cast<uint32_t*> (&__StackOneTop),
                              scb_hw->vtor);

  // Core 0's IPI channel, now that the launch handshake is over.
  fifo_drain ();
  NVIC_EnableIRQ (SIO_IRQ_PROC0_IRQn);
}

// ----------------------------------------------------------------------------

namespace
{
  // SIO inter-core FIFO IRQ, either core: drain the FIFO and request a
  // PendSV switch on this core.
  inline void
  sio_fifo_irq (void)
  {
    fifo_drain ();
    os::rtos::port::scheduler::reschedule ();
  }
} // namespace

extern "C" void
SIO_IRQ_PROC0_Handler (void)
{
  sio_fifo_irq ();
}

extern "C" void
SIO_IRQ_PROC1_Handler (void)
{
  sio_fifo_irq ();
}

// Core SysTick -> RTOS tick. Only core 0 advances the clock
// (os_systick_handler(), which itself ends by rescheduling); core 1's
// SysTick only reschedules core 1 so it picks up threads woken by core 0.
extern "C" void
SysTick_Handler (void)
{
  if (port_cpu_id () == 0)
    {
      os_systick_handler ();
    }
  else
    {
      os::rtos::port::scheduler::reschedule ();
    }
}

// ----------------------------------------------------------------------------

// PendSV_Handler: the context switch. Naked, so the LR/PSP handling is fully
// controlled; runs on the MSP. extern "C" so the SDK's vector table binds the
// C symbol, not the mangled C++ name.
extern "C" void __attribute__ ((naked, used, optimize ("1")))
PendSV_Handler (void)
{
  // Do NOT use push/pop, since the LR is from a different context.
  os::rtos::port::scheduler::restore_from_stack (
      os::rtos::port::scheduler::switch_stacks (
          os::rtos::port::scheduler::save_on_stack ()));

  asm volatile ("bx lr");
}

// ----------------------------------------------------------------------------
