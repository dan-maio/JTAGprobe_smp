# Toolchain: aarch64-none-elf — AArch64 (aarch64).
set (CMAKE_SYSTEM_PROCESSOR aarch64)
set (UOS_TOOLCHAIN_PREFIX "aarch64-none-elf-")
set (UOS_TOOLCHAIN_XPACK  "aarch64-none-elf-gcc")
include ("${CMAKE_CURRENT_LIST_DIR}/uos-bare-metal-common.cmake")
