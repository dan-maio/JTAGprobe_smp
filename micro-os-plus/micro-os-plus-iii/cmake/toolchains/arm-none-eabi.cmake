# Toolchain: arm-none-eabi — Cortex-M (cortexm) and AArch32 (aarch32).
set (CMAKE_SYSTEM_PROCESSOR arm)
set (UOS_TOOLCHAIN_PREFIX "arm-none-eabi-")
set (UOS_TOOLCHAIN_XPACK  "arm-none-eabi-gcc")
include ("${CMAKE_CURRENT_LIST_DIR}/uos-bare-metal-common.cmake")
