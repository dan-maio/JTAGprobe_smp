# -----------------------------------------------------------------------------
# uos-app.cmake — the single application-declaration helper.
#
# D7: the predecessor repository declared each application with its own
# Makefile, 214 of them, differing by roughly 48 lines of boilerplate. This
# file replaces that boilerplate, so a per-application CMakeLists.txt states
# only what is genuinely specific to it.
#
# Usage:
#
#   uos_add_app (smp_test0
#     SOURCES       ${_test_dir}/smp_test0/main.cpp
#     PORT          micro-os-plus::port-aarch32
#     LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
#     NCPU          4
#   )
# -----------------------------------------------------------------------------

include_guard (GLOBAL)

function (uos_add_app _name)
  set (_opts NO_KERNEL)
  set (_one  LINKER_SCRIPT PORT NCPU OUTPUT_NAME)
  set (_many SOURCES INCLUDES DEFINES OPTIONS LIBRARIES)
  cmake_parse_arguments (A "${_opts}" "${_one}" "${_many}" ${ARGN})

  if (NOT A_SOURCES)
    message (FATAL_ERROR "uos_add_app(${_name}): SOURCES is required")
  endif ()

  add_executable (${_name} ${A_SOURCES})

  # The kernel is linked unless the caller says NO_KERNEL; the port is whatever
  # architecture repo is driving this build. Everything else -- drivers,
  # silicon support -- comes through LIBRARIES, named by the caller.
  #
  # micro-os-plus::devices used to be linked here whenever the target existed,
  # which quietly compiled the SD/USB drivers into applications that never
  # asked for them. That is invisible on a board those drivers happen to
  # support and a build failure on one they do not, so the caller names them.
  #
  # NO_KERNEL exists for a small, real category: the bare-metal probes that run
  # BEFORE any scheduler does. The RP2350 has two -- smp-test0, which proves
  # the second core starts and that the two can talk, and exc-test, which
  # asks whether exception ENTRY works at all. Their value is precisely that
  # they run without a kernel, so they must not be rewritten as RTOS tests,
  # and linking one under them would change what they prove. Such an
  # application links its board'"'"'s startup and its own main(), nothing else:
  # no kernel, no port scheduler, no OS_NCPU -- there is nothing there to
  # configure.
  if (NOT A_NO_KERNEL)
    target_link_libraries (${_name} PRIVATE micro-os-plus::iii)
  endif ()
  if (A_PORT)
    target_link_libraries (${_name} PRIVATE ${A_PORT})
  endif ()
  if (A_LIBRARIES)
    target_link_libraries (${_name} PRIVATE ${A_LIBRARIES})
  endif ()

  if (A_INCLUDES)
    target_include_directories (${_name} PRIVATE ${A_INCLUDES})
  endif ()
  if (A_DEFINES)
    target_compile_definitions (${_name} PRIVATE ${A_DEFINES})
  endif ()
  if (A_OPTIONS)
    target_compile_options (${_name} PRIVATE ${A_OPTIONS})
  endif ()

  # The flags every bare-metal application in the predecessor repository
  # carried in its own Makefile, identically. The architecture project adds
  # only its -mcpu/-march on top of these.
  if (CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_compile_options (
      ${_name} PRIVATE
      -O2 -g3 -fmessage-length=0 -fsigned-char
      -ffunction-sections -fdata-sections
      $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
      $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
      $<$<COMPILE_LANGUAGE:CXX>:-fabi-version=0>
      $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
      $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
      $<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>
    )
    target_link_options (
      ${_name} PRIVATE
      -nostartfiles
      -Wl,--gc-sections
      "-Wl,-Map,$<TARGET_FILE_DIR:${_name}>/${_name}.map"
    )
  endif ()

  # OS_NCPU is how a board selects its CPU count from a shared port (D11:
  # the STM32 boards build the SMP cortexm port at OS_NCPU=1). It configures
  # the kernel, so an application without one does not get it -- smp-test0
  # starts the second core itself, with its own code.
  if (A_NCPU AND NOT A_NO_KERNEL)
    target_compile_definitions (${_name} PRIVATE OS_NCPU=${A_NCPU})
    if (A_NCPU GREATER 1)
      target_compile_definitions (${_name} PRIVATE OS_USE_SMP_SCHEDULER=1)
    endif ()
  endif ()

  if (A_OUTPUT_NAME)
    set_target_properties (${_name} PROPERTIES OUTPUT_NAME "${A_OUTPUT_NAME}")
  endif ()

  if (A_LINKER_SCRIPT)
    target_link_options (${_name} PRIVATE "-T${A_LINKER_SCRIPT}")
    set_target_properties (${_name} PROPERTIES LINK_DEPENDS "${A_LINKER_SCRIPT}")
  endif ()

  # Bare-metal targets want a raw image and a listing beside the ELF.
  if (CMAKE_SYSTEM_NAME STREQUAL "Generic")
    add_custom_command (
      TARGET ${_name} POST_BUILD
      COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:${_name}>"
              "$<TARGET_FILE_DIR:${_name}>/${_name}.bin"
      COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${_name}>"
      VERBATIM
    )
  endif ()
endfunction ()
