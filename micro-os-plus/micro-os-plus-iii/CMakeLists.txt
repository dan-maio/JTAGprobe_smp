# -----------------------------------------------------------------------------
# micro-os-plus-iii-smp — the common half of the unified µOS++ III SMP project.
#
# This repository is the RTOS kernel (src/, include/) and the runners that
# drive a build on QEMU or on hardware (test_smpl/). Nothing else.
#
# The test APPLICATIONS are not here. Every board owns its own, in
# boards/<id>/test/ of the architecture project that has that board, because a
# board is what decides which tests make sense on it and what they are built
# with.
#
# Architecture-specific code lives in its own INDEPENDENT repository, outside
# this one — micro-os-plus-iii-cortexm, -aarch32, -aarch64, -posix-arch. The
# dependency runs one way only: an architecture project consumes this one.
#
#   add_subdirectory (micro-os-plus-iii-smp)       # or a submodule of it
#   target_link_libraries (my-app PRIVATE micro-os-plus::iii)
#
# Nothing here may depend on an architecture project.
#
# Portions derived from µOS++ IIIe by Liviu Ionescu, MIT licensed; see
# LICENSE-micro-os-plus-iii.
# -----------------------------------------------------------------------------

cmake_minimum_required (VERSION 3.20)

project (
  micro-os-plus-iii-smp
  DESCRIPTION "µOS++ III SMP — common kernel, devices and tests"
  LANGUAGES C CXX ASM
)

# True only when this repo is the top-level build, false when an architecture
# project consumes it.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set (UOS_SMP_STANDALONE TRUE)
else ()
  set (UOS_SMP_STANDALONE FALSE)
endif ()

# Absolute paths are forbidden; consumers use these instead.
set (UOS_SMP_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
set (UOS_CMAKE_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/cmake" CACHE INTERNAL "")
set (UOS_TOOLCHAINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains" CACHE INTERNAL "")

include ("${CMAKE_CURRENT_SOURCE_DIR}/cmake/uos-app.cmake")

# -----------------------------------------------------------------------------
# Return early if micro-os-plus::iii has already been defined by a parent or sibling.
if (TARGET micro-os-plus::iii)
  return ()
endif ()

# -----------------------------------------------------------------------------
# The kernel — an INTERFACE library, compiled by whichever application links it
# against that application's architecture port.
#
# It does NOT compile on its own: include/cmsis-plus/rtos/os-decls.h includes
# <cmsis-plus/rtos/port/os-decls.h>, which an architecture project supplies.

add_library (micro-os-plus-iii-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-interface INTERFACE "include" "include/cmsis-plus/legacy"
)

# The core: what every port compiles. Measured against the proven rpi
# builds, which took exactly this set and nothing else.
target_sources (
  micro-os-plus-iii-interface
  INTERFACE
        src/diag/trace.cpp
        src/libcpp/chrono.cpp
        src/libcpp/condition-variable.cpp
        src/libcpp/cxx.cpp
        src/libcpp/memory-resource.cpp
        src/libcpp/mutex.cpp
        src/libcpp/new.cpp
        src/libcpp/system-error.cpp
        src/libcpp/thread.cpp
        src/libc/_sbrk.c
        src/libc/stdlib/assert.c
        src/libc/stdlib/atexit.cpp
        src/libc/stdlib/exit.c
        src/libc/stdlib/init-fini.c
        src/libc/stdlib/malloc.cpp
        src/libc/stdlib/timegm.c
        src/memory/block-pool.cpp
        src/memory/first-fit-top.cpp
        src/memory/lifo.cpp
        src/rtos/internal/os-flags.cpp
        src/rtos/internal/os-lists.cpp
        src/rtos/os-clocks.cpp
        src/rtos/os-condvar.cpp
        src/rtos/os-core.cpp
        src/rtos/os-c-wrapper.cpp
        src/rtos/os-evflags.cpp
        src/rtos/os-idle.cpp
        src/rtos/os-main.cpp
        src/rtos/os-memory.cpp
        src/rtos/os-mempool.cpp
        src/rtos/os-mqueue.cpp
        src/rtos/os-mutex.cpp
        src/rtos/os-semaphore.cpp
        src/rtos/os-thread.cpp
        src/rtos/os-timer.cpp
        src/startup/initialise-free-store.cpp
        src/utils/lists.cpp
)

add_library (micro-os-plus::iii ALIAS micro-os-plus-iii-interface)
add_library (micro-os-plus::iii-core ALIAS micro-os-plus-iii-interface)

# -----------------------------------------------------------------------------
# Optional groups. Upstream ships these beside the core, but a bare-metal port
# does not necessarily want them -- the rpi ports compile none of them: they
# have their own startup.S, their own semihosting in uart.hpp, and no file
# descriptors. Listing them in the core target is what made
# c-syscalls-posix.cpp collide with newlib's unistd.h.
#
# A port takes what it needs:
#
#   target_link_libraries (my-port INTERFACE micro-os-plus::iii-posix-io)
#
# The trace backends are mutually exclusive: link at most one.

add_library (micro-os-plus-iii-posix-io-interface INTERFACE)
target_sources (micro-os-plus-iii-posix-io-interface INTERFACE
        src/posix-io/block-device-partition.cpp
        src/posix-io/block-device.cpp
        src/posix-io/c-syscalls-posix.cpp
        src/posix-io/char-device.cpp
        src/posix-io/device.cpp
        src/posix-io/directory.cpp
        src/posix-io/file-descriptors-manager.cpp
        src/posix-io/file-system.cpp
        src/posix-io/file.cpp
        src/posix-io/io.cpp
        src/posix-io/net-stack.cpp
        src/posix-io/socket.cpp
        src/posix-io/tty.cpp
)
add_library (micro-os-plus::iii-posix-io ALIAS micro-os-plus-iii-posix-io-interface)

add_library (micro-os-plus-iii-drivers-interface INTERFACE)
target_sources (micro-os-plus-iii-drivers-interface INTERFACE
        src/driver/common.cpp
        src/driver/serial.cpp
)
add_library (micro-os-plus::iii-drivers ALIAS micro-os-plus-iii-drivers-interface)

add_library (micro-os-plus-iii-startup-interface INTERFACE)
target_sources (micro-os-plus-iii-startup-interface INTERFACE
        src/startup/exception-handlers.c
        src/startup/initialize-hardware.c
        src/startup/reset-hardware.c
        src/startup/startup.cpp
)
add_library (micro-os-plus::iii-startup ALIAS micro-os-plus-iii-startup-interface)

add_library (micro-os-plus-iii-newlib-reent-interface INTERFACE)
target_sources (micro-os-plus-iii-newlib-reent-interface INTERFACE
        src/libc/newlib/c-newlib-reent.cpp
)
add_library (micro-os-plus::iii-newlib-reent ALIAS micro-os-plus-iii-newlib-reent-interface)

add_library (micro-os-plus-iii-semihosting-interface INTERFACE)
target_sources (micro-os-plus-iii-semihosting-interface INTERFACE
        src/semihosting/c-syscalls-semihosting.cpp
)
add_library (micro-os-plus::iii-semihosting ALIAS micro-os-plus-iii-semihosting-interface)

add_library (micro-os-plus-iii-trace-itm-interface INTERFACE)
target_sources (micro-os-plus-iii-trace-itm-interface INTERFACE
        src/diag/trace-itm.cpp
)
add_library (micro-os-plus::iii-trace-itm ALIAS micro-os-plus-iii-trace-itm-interface)

add_library (micro-os-plus-iii-trace-semihosting-interface INTERFACE)
target_sources (micro-os-plus-iii-trace-semihosting-interface INTERFACE
        src/diag/trace-semihosting.cpp
)
add_library (micro-os-plus::iii-trace-semihosting ALIAS micro-os-plus-iii-trace-semihosting-interface)

add_library (micro-os-plus-iii-trace-segger-rtt-interface INTERFACE)
target_sources (micro-os-plus-iii-trace-segger-rtt-interface INTERFACE
        src/diag/trace-segger-rtt.cpp
)
add_library (micro-os-plus::iii-trace-segger-rtt ALIAS micro-os-plus-iii-trace-segger-rtt-interface)

# -----------------------------------------------------------------------------
# Drivers and SoC support are NOT part of this repository. They live in
# micro-os-plus-iii-devices, and an architecture project adds them only when
# its build needs them:
#
#   add_subdirectory (micro-os-plus-iii-smp)        # always
#   add_subdirectory (micro-os-plus-iii-devices)    # only if needed
#
# Keeping them out is what lets this repository stay merge-clean against
# upstream micro-os-plus-iii (D3): src/ and include/ match upstream
# path-for-path, so the only conflicts are the six genuine SMP files.

# -----------------------------------------------------------------------------
# Optional: the C++ half of the SMP port contract, shared by the ARM ports.
# Opt-in -- a port links this instead of writing its own os-decls.h. See
# port/smp-common/README.md.
add_library (micro-os-plus-iii-port-smp-decls-interface INTERFACE)
target_include_directories (
  micro-os-plus-iii-port-smp-decls-interface INTERFACE "port/smp-common"
)
add_library (micro-os-plus::port-smp-decls ALIAS micro-os-plus-iii-port-smp-decls-interface)

# -----------------------------------------------------------------------------
# Optional: the test scaffolding both silicon ports need and neither owns.
# Opt-in, exactly like port-smp-decls above, and for the same reason: these
# were duplicate copies in two repositories, which is what
# tools/verify-no-duplicate-sources.py exists to catch.
#
#   test_smpl/include/hw_result.hpp      how a test ends a run with a verdict
#   test_smpl/src/board-contract.cpp     no code; turns "this board never said
#                                        X" into a compile error rather than a
#                                        silent inheritance of the previous
#                                        board's answer
#
# Only the HEADER comes through this target. board-contract.cpp is named
# directly by each port, in the position its own copy used to occupy in the
# source list -- an INTERFACE source would be appended somewhere else and
# would change link order, and the point of this move is that it changes
# nothing: all 24 images per board stay byte-identical across it.
#
# The POSIX host port uses neither: no semihosting, and no board contract of
# this shape.
add_library (micro-os-plus-iii-test-support-interface INTERFACE)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/smp-support/include")
  target_include_directories (
    micro-os-plus-iii-test-support-interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/smp-support/include"
  )
else ()
  target_include_directories (
    micro-os-plus-iii-test-support-interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_smpl/include"
  )
endif ()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/smp-support/src/board-contract.cpp")
  target_sources (micro-os-plus-iii-test-support-interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/smp-support/src/board-contract.cpp")
else ()
  target_sources (micro-os-plus-iii-test-support-interface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/test_smpl/src/board-contract.cpp")
endif ()
add_library (micro-os-plus::test-support ALIAS micro-os-plus-iii-test-support-interface)

# -----------------------------------------------------------------------------
message (STATUS "µOS++ III SMP common: micro-os-plus::iii  standalone=${UOS_SMP_STANDALONE}")
