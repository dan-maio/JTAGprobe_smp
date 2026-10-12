# -----------------------------------------------------------------------------
# micro-os-plus-iii-rp2040 -- the RP2040 (2x Cortex-M0+) SMP port of µOS++ III,
# for an application that boots with the Raspberry Pi Pico SDK.
#
# Built the way micro-os-plus-iii-cortexm is: the kernel is an INTERFACE
# library (micro-os-plus::iii), the port is an INTERFACE library on top of it
# (micro-os-plus::rp2040), and the application links the port. Nothing here is
# compiled until an executable links it, and then it is compiled with that
# executable's flags -- the Pico SDK's -mcpu=cortex-m0plus.
#
# What differs from the RP2350 port (micro-os-plus-iii-cortexm,
# src/rtos/os-core-rp2350.cpp, include-rp2350/), and why:
#
#   ISA           ARMv6-M: no BASEPRI, no stmdb/ldmia of r8-r11, no IT blocks.
#                 Critical sections are PRIMASK; the context switch saves
#                 r8-r11 through the low registers (as upstream's Cortex-M
#                 core does for the M0).
#   kernel lock   SIO hardware spinlock PICO_SPINLOCK_ID_OS1 (14), the one the
#                 Pico SDK reserves for an operating system. Not spinlock 0:
#                 here the SDK runs beside the kernel and owns the low ones.
#   IPI           the SIO inter-core FIFO; on the RP2040 each core has its own
#                 IRQ line, SIO_IRQ_PROC0 (15) and SIO_IRQ_PROC1 (16).
#   boot          the Pico SDK's crt0 and runtime, not a boot.S of our own.
#                 The SDK's exception names are renamed to the CMSIS ones
#                 (cmsis_core, PICO_CMSIS_RENAME_EXCEPTIONS), so the strong
#                 PendSV_Handler / SysTick_Handler / SIO_IRQ_PROCn_Handler here
#                 replace the SDK's weak vectors.
#   core 1        launched with the SDK's multicore_launch_core1_raw() on the
#                 SDK's core-1 stack (SCRATCH_X, __StackOneTop).
#
# Exports: micro-os-plus::rp2040
# -----------------------------------------------------------------------------
cmake_minimum_required (VERSION 3.20)

# -----------------------------------------------------------------------------
# The kernel. Sibling directory by default, exactly as the architecture ports
# find it; override with -DUOS_SMP_DIR=... to build against another copy.
get_filename_component (_uos_siblings "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set (UOS_SMP_DIR "${_uos_siblings}/micro-os-plus-iii"
     CACHE PATH "µOS++ III SMP kernel working copy")

if (NOT TARGET micro-os-plus::iii)
  if (NOT EXISTS "${UOS_SMP_DIR}/CMakeLists.txt")
    message (FATAL_ERROR
      "Cannot find the µOS++ III SMP kernel at ${UOS_SMP_DIR}.\n"
      "Point at it with -DUOS_SMP_DIR=/path/to/micro-os-plus-iii")
  endif ()
  add_subdirectory ("${UOS_SMP_DIR}" micro-os-plus-iii)
endif ()

# Process exit belongs to the Pico SDK runtime here: its newlib interface
# (pico_clib_interface/newlib_interface.c) defines a strong exit(), and so
# does the kernel's src/libc/stdlib/exit.c. The kernel tree is left as it is;
# the port takes that one file out of what the kernel target hands to the
# executable.
get_target_property (_uos_kernel_sources micro-os-plus-iii-interface
                     INTERFACE_SOURCES)
list (FILTER _uos_kernel_sources EXCLUDE REGEX "/src/libc/stdlib/exit\\.c$")
set_property (TARGET micro-os-plus-iii-interface
              PROPERTY INTERFACE_SOURCES "${_uos_kernel_sources}")

# -----------------------------------------------------------------------------
add_library (micro-os-plus-iii-rp2040-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-rp2040-interface
  INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_sources (
  micro-os-plus-iii-rp2040-interface
  INTERFACE
        # The PendSV context switch, the per-core SysTick, the kernel lock,
        # the IPI and the core-1 launch.
        "${CMAKE_CURRENT_LIST_DIR}/src/os-core-rp2040.cpp"
)

# Two CPUs, and the kernel's SMP scheduler. In the micro-os-plus-iii-smp tests
# uos_add_app() derives these per application from the board's NCPU; this
# port has one board and one CPU count, so it states them once, here.
target_compile_definitions (
  micro-os-plus-iii-rp2040-interface
  INTERFACE OS_NCPU=2 OS_USE_SMP_SCHEDULER=1
)

target_compile_options (
  micro-os-plus-iii-rp2040-interface
  INTERFACE
        # As the Cortex-M port: without it every static object with a
        # destructor registers through __cxa_atexit.
        $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
)

target_link_options (
  micro-os-plus-iii-rp2040-interface
  INTERFACE
        # The kernel's _sbrk() (src/libc/_sbrk.c) reads these; the Pico SDK's
        # linker script names the same region __end__ .. __HeapLimit.
        "LINKER:--defsym=_Heap_Begin=__end__"
        "LINKER:--defsym=_Heap_Limit=__HeapLimit"
)

target_link_libraries (
  micro-os-plus-iii-rp2040-interface
  INTERFACE
        micro-os-plus::iii
        # Pico SDK: the CMSIS device header (RP2040.h, SystemCoreClock) and
        # the exception renaming; the SIO and spinlock registers; the timer
        # for the high-resolution clock; the core-1 launch.
        cmsis_core
        hardware_sync
        hardware_timer
        hardware_clocks
        pico_multicore
)

add_library (micro-os-plus::rp2040 ALIAS micro-os-plus-iii-rp2040-interface)

message (STATUS "µOS++ III RP2040 port: micro-os-plus::rp2040 (2x Cortex-M0+ SMP)")
