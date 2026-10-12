# Shared preamble for every bare-metal toolchain file.
#
# D7: the three toolchain files differ only in triple and processor, so
# everything else lives here once. Include it after setting UOS_TOOLCHAIN_PREFIX
# and CMAKE_SYSTEM_PROCESSOR.

set (CMAKE_SYSTEM_NAME Generic)

# Bare-metal toolchains cannot link a bare executable during compiler
# verification, so probe with a static library instead.
set (CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Prefer an xPack toolchain when one is installed, else fall back to PATH.
# -DUOS_TOOLCHAIN_BIN=<dir> pins one exactly, which is what you want when
# bisecting a miscompilation across toolchain versions.
if (UOS_TOOLCHAIN_BIN)
  set (_uos_xpacks "${UOS_TOOLCHAIN_BIN}")
else ()
file (GLOB _uos_xpacks
      "$ENV{HOME}/.local/xPacks/@xpack-dev-tools/${UOS_TOOLCHAIN_XPACK}/*/.content/bin"
)
endif ()
if (_uos_xpacks)
  list (SORT _uos_xpacks COMPARE NATURAL ORDER DESCENDING)
  list (GET _uos_xpacks 0 _uos_bin)
  set (_uos_prefix "${_uos_bin}/${UOS_TOOLCHAIN_PREFIX}")
else ()
  set (_uos_prefix "${UOS_TOOLCHAIN_PREFIX}")
endif ()

set (CMAKE_C_COMPILER   "${_uos_prefix}gcc")
set (CMAKE_CXX_COMPILER "${_uos_prefix}g++")
set (CMAKE_ASM_COMPILER "${_uos_prefix}gcc")

set (CMAKE_OBJCOPY "${_uos_prefix}objcopy" CACHE INTERNAL "objcopy")
set (CMAKE_OBJDUMP "${_uos_prefix}objdump" CACHE INTERNAL "objdump")
set (CMAKE_SIZE    "${_uos_prefix}size"    CACHE INTERNAL "size")

set (CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set (CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
