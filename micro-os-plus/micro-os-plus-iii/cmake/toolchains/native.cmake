# Toolchain: native host — posix-arch.
#
# Deliberately minimal: the host compiler needs no cross-compilation setup,
# and CMAKE_SYSTEM_NAME must NOT be set, or CMake treats this as cross-building.
set (CMAKE_C_COMPILER   "cc"  CACHE STRING "")
set (CMAKE_CXX_COMPILER "c++" CACHE STRING "")
