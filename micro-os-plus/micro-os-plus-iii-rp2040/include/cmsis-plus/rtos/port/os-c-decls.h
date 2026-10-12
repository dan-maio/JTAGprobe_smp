/*
 * os-c-decls.h -- port-specific C declarations for the RP2040 (2x Cortex-M0+)
 * SMP port of the shared micro-os-plus-iii kernel.
 *
 * Derived from the RP2350 port's (micro-os-plus-iii-cortexm,
 * include-rp2350/cmsis-plus/rtos/port/os-c-decls.h). Same types; the CPU
 * count comes from the port's CMake (OS_NCPU=2), as uos_add_app() passes it
 * in the micro-os-plus-iii-smp tests.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t os_port_clock_timestamp_t;
typedef uint32_t os_port_clock_duration_t;
typedef uint64_t os_port_clock_offset_t;

typedef bool os_port_scheduler_state_t;

typedef uint32_t os_port_irq_state_t;

typedef uint32_t os_port_thread_stack_element_t;
typedef uint64_t os_port_thread_stack_allocation_element_t;

typedef struct
{
  os_port_thread_stack_element_t* stack_ptr;
} os_port_thread_context_t;

/* Cross-core reschedule is signalled over the SIO inter-core FIFO: the
 * sending core pushes a word, the receiving core's SIO_IRQ_PROCn drains it
 * and pends its own PendSV. */
#define OS_SMP_IPI_FIFO 1

#if defined(OS_USE_SMP_SCHEDULER)

#if !defined(OS_NCPU)
#define OS_NCPU 2
#endif

/* SMP scheduler lock (recursive). The hardware lock word is the SIO spinlock
 * PICO_SPINLOCK_ID_OS1; here only the recursive owner/depth are tracked. */
typedef struct
{
  volatile uint32_t owner;
  volatile uint32_t depth;
} os_port_smp_klock_t;

#define SMP_NO_OWNER 0xFFFFFFFFu

#endif /* defined(OS_USE_SMP_SCHEDULER) */

#define OS_INTEGER_RTOS_STACK_FILL_MAGIC (0xEFBEADDE)

#define OS_HAS_INTERRUPTS_STACK

#endif /* CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_ */
