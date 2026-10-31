# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)

project(sigrok_bridge LANGUAGES C CXX)

if(NOT MSVC AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The bridge is built with MSVC on Windows and with GCC or Clang on Linux (README.md).")
endif()

if(MSVC)
  set(SIGROK_BRIDGE_VCPKG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg_installed/x64-windows" CACHE PATH
      "vcpkg's installed tree of glib, libzip and libusb (x64-windows): what scripts/install_msvc_dlls.cmd made")
  if(NOT EXISTS "${SIGROK_BRIDGE_VCPKG_DIR}/lib/glib-2.0.lib" OR NOT EXISTS "${SIGROK_BRIDGE_VCPKG_DIR}/lib/libusb-1.0.lib")
    message(FATAL_ERROR "No glib and libusb in ${SIGROK_BRIDGE_VCPKG_DIR}: run scripts/install_msvc_dlls.cmd "
                        "(README.md).")
  endif()

  set(sigrok_bridge_vcpkg_dlls
      glib-2.0-0 intl-8 iconv-2 pcre2-8 zip z bz2 libusb-1.0)

  add_library(sigrok_bridge_glib SHARED IMPORTED GLOBAL)
  set_target_properties(sigrok_bridge_glib PROPERTIES
      IMPORTED_IMPLIB "${SIGROK_BRIDGE_VCPKG_DIR}/lib/glib-2.0.lib"
      IMPORTED_LOCATION "${SIGROK_BRIDGE_VCPKG_DIR}/bin/glib-2.0-0.dll")
  target_include_directories(sigrok_bridge_glib SYSTEM INTERFACE
      "${SIGROK_BRIDGE_VCPKG_DIR}/include/glib-2.0"
      "${SIGROK_BRIDGE_VCPKG_DIR}/lib/glib-2.0/include")

  add_library(sigrok_bridge_libusb_dll SHARED IMPORTED GLOBAL)
  set_target_properties(sigrok_bridge_libusb_dll PROPERTIES
      IMPORTED_IMPLIB "${SIGROK_BRIDGE_VCPKG_DIR}/lib/libusb-1.0.lib"
      IMPORTED_LOCATION "${SIGROK_BRIDGE_VCPKG_DIR}/bin/libusb-1.0.dll")
  add_library(sigrok_bridge_libusb INTERFACE)
  target_include_directories(sigrok_bridge_libusb SYSTEM INTERFACE "${SIGROK_BRIDGE_VCPKG_DIR}/include")
  target_link_libraries(sigrok_bridge_libusb INTERFACE sigrok_bridge_libusb_dll)
  add_library(bitoscope_libusb INTERFACE)
  target_include_directories(bitoscope_libusb SYSTEM INTERFACE "${SIGROK_BRIDGE_VCPKG_DIR}/include/libusb-1.0")
  target_link_libraries(bitoscope_libusb INTERFACE sigrok_bridge_libusb_dll)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SIGROK_BRIDGE_GLIB_PC REQUIRED IMPORTED_TARGET glib-2.0)
  add_library(sigrok_bridge_glib INTERFACE)
  target_link_libraries(sigrok_bridge_glib INTERFACE PkgConfig::SIGROK_BRIDGE_GLIB_PC)
  # WHY: libusb only through the shim's link interface: it then follows the shim, and the shim keeps interposing.
  add_library(sigrok_bridge_libusb INTERFACE)
  target_link_libraries(sigrok_bridge_libusb INTERFACE bridge_usb)
  add_library(bitoscope_libusb INTERFACE)
  target_link_libraries(bitoscope_libusb INTERFACE bridge_usb)
endif()

include(cmake/BitoscopeBuildJobs.cmake)

add_subdirectory(sdk)

include(cmake/BitoscopeOwnTargetWarnings.cmake)

add_subdirectory(usbshim)
add_library(sigrok_bridge_usb_shim INTERFACE)
target_link_libraries(sigrok_bridge_usb_shim INTERFACE bridge_usb)

if(MSVC)

  option(SIGROK_BRIDGE_USE_VENDORED "Build against vendor/sigrok-msvc (libsigrok's headers and .def file, no DLLs)" OFF)
  set(sigrok_bridge_full_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-msvc")
  set(sigrok_bridge_vendored_dir "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sigrok-msvc")
  if(SIGROK_BRIDGE_USE_VENDORED OR NOT EXISTS "${sigrok_bridge_full_dir}/lib/libsigrok-4.def")
    set(sigrok_bridge_default_dir "${sigrok_bridge_vendored_dir}")
  else()
    set(sigrok_bridge_default_dir "${sigrok_bridge_full_dir}")
  endif()
  set(SIGROK_MSVC_DIR "${sigrok_bridge_default_dir}" CACHE PATH
      "What scripts/build_libsigrok.sh put together (include/, lib/libsigrok-4.def and bin/*.dll), or vendor/sigrok-msvc")
  # WHY: MSBuild reads "%XX" in a path as a hex escape, so a "%" in this folder breaks the build commands.
  if(SIGROK_MSVC_DIR MATCHES "%|\\$PWD")
    message(WARNING "SIGROK_MSVC_DIR '${SIGROK_MSVC_DIR}' holds a variable the shell did not expand (%CD% works "
                    "only in cmd, $PWD only in PowerShell and sh): using ${sigrok_bridge_default_dir} instead.")
    set(SIGROK_MSVC_DIR "${sigrok_bridge_default_dir}" CACHE PATH "" FORCE)
  # WHY: A cached default must follow: build_libsigrok.sh makes third_party/sigrok-msvc after the first configure.
  elseif(SIGROK_BRIDGE_USE_VENDORED OR SIGROK_MSVC_DIR STREQUAL sigrok_bridge_vendored_dir
         OR SIGROK_MSVC_DIR STREQUAL sigrok_bridge_full_dir)
    set(SIGROK_MSVC_DIR "${sigrok_bridge_default_dir}" CACHE PATH "" FORCE)
  endif()
  if(NOT EXISTS "${SIGROK_MSVC_DIR}/lib/libsigrok-4.def")
    message(FATAL_ERROR "No libsigrok headers and .def file in ${SIGROK_MSVC_DIR}: they are in "
                        "vendor/sigrok-msvc, or run scripts/build_libsigrok.sh in the MSYS2 UCRT64 shell "
                        "(README.md).")
  endif()
  if(EXISTS "${SIGROK_MSVC_DIR}/bin/libsigrok-4.dll")
    set(SIGROK_BRIDGE_HAVE_RUNTIME TRUE)
  else()
    set(SIGROK_BRIDGE_HAVE_RUNTIME FALSE)
    message(STATUS "No libsigrok-4.dll in ${SIGROK_MSVC_DIR}/bin: the bridge is built and linked but cannot "
                   "run")
  endif()

  if(NOT DEFINED SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR)
    set(SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsigrok")
    if(EXISTS "${SIGROK_MSVC_DIR}/libsigrok-source.txt")
      file(STRINGS "${SIGROK_MSVC_DIR}/libsigrok-source.txt" sigrok_bridge_source_lines LIMIT_COUNT 1)
      if(sigrok_bridge_source_lines)
        list(GET sigrok_bridge_source_lines 0 SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR)
      endif()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SIGROK_MSVC_DIR}/libsigrok-source.txt")
    endif()
  endif()
  message(STATUS "libsigrok was built from ${SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR}")

  function(sigrok_bridge_import_dll name dll_name)
    set(definition "${SIGROK_MSVC_DIR}/lib/${dll_name}.def")
    set(import_library "${CMAKE_CURRENT_BINARY_DIR}/import/${dll_name}.lib")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${definition}")
    if(NOT EXISTS "${import_library}" OR "${definition}" IS_NEWER_THAN "${import_library}")
      file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/import")
      execute_process(
          COMMAND "${CMAKE_AR}" /nologo "/def:${definition}" /machine:x64 "/out:${import_library}"
          RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
      if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not make ${import_library}: ${output}")
      endif()
    endif()
    add_library(${name} SHARED IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES
        IMPORTED_IMPLIB "${import_library}"
        IMPORTED_LOCATION "${SIGROK_MSVC_DIR}/bin/${dll_name}.dll")
  endfunction()

  sigrok_bridge_import_dll(sigrok_bridge_libsigrok libsigrok-4)

  add_library(sigrok_bridge_sigrok INTERFACE)
  target_include_directories(sigrok_bridge_sigrok SYSTEM INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc"
      "${SIGROK_MSVC_DIR}/include")
  target_link_libraries(sigrok_bridge_sigrok INTERFACE sigrok_bridge_libsigrok sigrok_bridge_glib bridge_usb_headers)
else()
  set(SIGROK_BRIDGE_LIBSIGROK_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/install-libsigrok-linux" CACHE PATH
      "The install prefix of libsigrok (lib/libsigrok.so, include/libsigrok/): what scripts/build_libsigrok_linux.sh made")
  file(GLOB sigrok_bridge_libsigrok_candidates "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/lib/libsigrok.so"
       "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/lib64/libsigrok.so")
  if(NOT sigrok_bridge_libsigrok_candidates OR NOT EXISTS "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/include/libsigrok/libsigrok.h")
    message(FATAL_ERROR "No libsigrok in ${SIGROK_BRIDGE_LIBSIGROK_PREFIX}: run scripts/build_libsigrok_linux.sh "
                        "(README.md, \"Building on Linux\"), or name a prefix with "
                        "-DSIGROK_BRIDGE_LIBSIGROK_PREFIX=<dir>.")
  endif()
  list(GET sigrok_bridge_libsigrok_candidates 0 sigrok_bridge_libsigrok_so)
  get_filename_component(sigrok_bridge_libsigrok_so "${sigrok_bridge_libsigrok_so}" REALPATH)
  get_filename_component(SIGROK_BRIDGE_LIBSIGROK_LIBDIR "${sigrok_bridge_libsigrok_so}" DIRECTORY)
  set(SIGROK_MSVC_DIR "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}")
  set(SIGROK_BRIDGE_HAVE_RUNTIME TRUE)

  if(NOT DEFINED SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR)
    set(SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsigrok")
    if(EXISTS "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/libsigrok-source.txt")
      file(STRINGS "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/libsigrok-source.txt" sigrok_bridge_source_lines LIMIT_COUNT 1)
      if(sigrok_bridge_source_lines)
        list(GET sigrok_bridge_source_lines 0 SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR)
      endif()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/libsigrok-source.txt")
    endif()
  endif()
  message(STATUS "libsigrok ${sigrok_bridge_libsigrok_so}, built from ${SIGROK_BRIDGE_LIBSIGROK_SOURCE_DIR}")

  add_library(sigrok_bridge_libsigrok SHARED IMPORTED GLOBAL)
  set_target_properties(sigrok_bridge_libsigrok PROPERTIES
      IMPORTED_LOCATION "${sigrok_bridge_libsigrok_so}")
  execute_process(COMMAND readelf -d "${sigrok_bridge_libsigrok_so}" OUTPUT_VARIABLE sigrok_bridge_libsigrok_dynamic ERROR_QUIET)
  if(sigrok_bridge_libsigrok_dynamic MATCHES "Library soname: \\[([^]]+)\\]")
    set(SIGROK_BRIDGE_LIBSIGROK_SONAME "${CMAKE_MATCH_1}")
    set_target_properties(sigrok_bridge_libsigrok PROPERTIES IMPORTED_SONAME "${CMAKE_MATCH_1}")
  endif()

  add_library(sigrok_bridge_sigrok INTERFACE)
  target_include_directories(sigrok_bridge_sigrok SYSTEM INTERFACE "${SIGROK_BRIDGE_LIBSIGROK_PREFIX}/include")
  target_link_libraries(sigrok_bridge_sigrok INTERFACE sigrok_bridge_libsigrok sigrok_bridge_glib bridge_usb_headers)
endif()

function(sigrok_bridge_compile_settings target)
  target_compile_features(${target} PUBLIC cxx_std_23)
  if(MSVC)
    # WHY: C4324 only reports padding an alignas asks for, GCC and Clang have no such warning (owner, 2026-10-21).
    target_compile_options(${target} PRIVATE /W4 /wd4324 /utf-8)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_library(sigrok_bridge_table STATIC src/usb_id_table.cpp)
target_include_directories(sigrok_bridge_table PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
sigrok_bridge_compile_settings(sigrok_bridge_table)

add_library(sigrok_bridge_listing STATIC src/listing_rules.cpp)
target_include_directories(sigrok_bridge_listing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sigrok_bridge_listing PUBLIC sigrok_bridge_table)
sigrok_bridge_compile_settings(sigrok_bridge_listing)

add_library(sigrok_bridge_device STATIC
    src/bridge_device.cpp
    src/device_scan.cpp
    src/sigrok_context.cpp
)
target_include_directories(sigrok_bridge_device PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sigrok_bridge_device PUBLIC
    bitoscope_driver_c_sdk sigrok_bridge_sigrok sigrok_bridge_usb_shim sigrok_bridge_table sigrok_bridge_listing)
sigrok_bridge_compile_settings(sigrok_bridge_device)

add_library(sigrok_bridge_threshold STATIC src/threshold.cpp)
target_link_libraries(sigrok_bridge_threshold PUBLIC sigrok_bridge_device)
sigrok_bridge_compile_settings(sigrok_bridge_threshold)

add_library(sigrok_bridge_pwm STATIC src/pwm_generator.cpp)
target_link_libraries(sigrok_bridge_pwm PUBLIC sigrok_bridge_device)
sigrok_bridge_compile_settings(sigrok_bridge_pwm)

add_library(sigrok_bridge_core_keeper STATIC src/core_keeper.cpp)
target_include_directories(sigrok_bridge_core_keeper PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
sigrok_bridge_compile_settings(sigrok_bridge_core_keeper)

add_library(sigrok_bridge_capture_rules STATIC src/capture_rules.cpp)
target_link_libraries(sigrok_bridge_capture_rules PUBLIC sigrok_bridge_table)
sigrok_bridge_compile_settings(sigrok_bridge_capture_rules)

add_library(sigrok_bridge_core STATIC
    src/bridge_driver.cpp
    src/capture.cpp
    src/device_entry.cpp
)
target_link_libraries(sigrok_bridge_core PUBLIC
    sigrok_bridge_device sigrok_bridge_threshold sigrok_bridge_pwm
    sigrok_bridge_capture_rules sigrok_bridge_core_keeper)
if(WIN32)
  target_link_libraries(sigrok_bridge_core PUBLIC winmm)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(sigrok_bridge_core PUBLIC Threads::Threads)
endif()
sigrok_bridge_compile_settings(sigrok_bridge_core)

add_executable(sigrok_bridge src/main.cpp)
target_link_libraries(sigrok_bridge PRIVATE sigrok_bridge_core)
sigrok_bridge_compile_settings(sigrok_bridge)
set_target_properties(sigrok_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/drivers"
)
if(MSVC)
  set(sigrok_bridge_driver_dir "${CMAKE_BINARY_DIR}/drivers/$<CONFIG>")
else()
  set(sigrok_bridge_driver_dir "${CMAKE_BINARY_DIR}/drivers")
endif()
if(MSVC)
  set(sigrok_bridge_vcpkg_dll_files "")
  foreach(dll IN LISTS sigrok_bridge_vcpkg_dlls)
    list(APPEND sigrok_bridge_vcpkg_dll_files "${SIGROK_BRIDGE_VCPKG_DIR}/bin/${dll}.dll")
  endforeach()
  add_custom_target(sigrok_bridge_dlls
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/drivers/$<CONFIG>"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${sigrok_bridge_vcpkg_dll_files}
              "$<TARGET_FILE:bridge_usb>" "${CMAKE_BINARY_DIR}/drivers/$<CONFIG>"
      VERBATIM)
  add_dependencies(sigrok_bridge_dlls bridge_usb)
  if(SIGROK_BRIDGE_HAVE_RUNTIME)
    add_custom_command(TARGET sigrok_bridge_dlls POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${SIGROK_MSVC_DIR}/bin" "${CMAKE_BINARY_DIR}/drivers/$<CONFIG>"
        VERBATIM)
  endif()
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-firmware")
    add_custom_command(TARGET sigrok_bridge_dlls POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-firmware" "${CMAKE_BINARY_DIR}/drivers/$<CONFIG>/sigrok-firmware"
        VERBATIM)
  endif()
  add_dependencies(sigrok_bridge sigrok_bridge_dlls)
else()
  set_target_properties(sigrok_bridge PROPERTIES BUILD_RPATH "$ORIGIN/lib" INSTALL_RPATH "$ORIGIN/lib")
  set(sigrok_bridge_dll_commands
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${sigrok_bridge_driver_dir}/lib"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${sigrok_bridge_libsigrok_so}"
              "${sigrok_bridge_driver_dir}/lib/${SIGROK_BRIDGE_LIBSIGROK_SONAME}")
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-firmware")
    list(APPEND sigrok_bridge_dll_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-firmware" "${sigrok_bridge_driver_dir}/sigrok-firmware")
  endif()
  add_custom_target(sigrok_bridge_dlls ${sigrok_bridge_dll_commands} VERBATIM)
  add_dependencies(sigrok_bridge_dlls bridge_usb)
endif()
add_dependencies(sigrok_bridge sigrok_bridge_dlls)

install(TARGETS sigrok_bridge RUNTIME DESTINATION drivers)
if(MSVC)
  install(TARGETS bridge_usb RUNTIME DESTINATION drivers)
  install(FILES ${sigrok_bridge_vcpkg_dll_files} DESTINATION drivers)
  if(SIGROK_BRIDGE_HAVE_RUNTIME)
    install(DIRECTORY "${SIGROK_MSVC_DIR}/bin/" DESTINATION drivers FILES_MATCHING PATTERN "*.dll")
  endif()
else()
  install(TARGETS bridge_usb LIBRARY DESTINATION drivers/lib)
  install(FILES "${sigrok_bridge_libsigrok_so}" DESTINATION drivers/lib RENAME "${SIGROK_BRIDGE_LIBSIGROK_SONAME}")
endif()
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-firmware")
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sigrok-firmware" DESTINATION drivers)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/private/CMakeLists.txt")
  # WHY: Here, not in the subdirectory: ctest looks for its test file in the top build directory.
  enable_testing()
  add_subdirectory(private)
endif()

# WHY: Last, after every add_subdirectory: a target added below this line would get no warnings.
bitoscope_enable_warnings_on_own_targets("${CMAKE_SOURCE_DIR}")
