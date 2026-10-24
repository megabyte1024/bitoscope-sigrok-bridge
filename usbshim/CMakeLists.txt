# SPDX-License-Identifier: GPL-3.0-or-later

if(MSVC)
  set(sigrok_bridge_shim_libusb_dll "${SIGROK_BRIDGE_VCPKG_DIR}/bin/libusb-1.0.dll")
  find_program(SIGROK_BRIDGE_DUMPBIN dumpbin HINTS "${CMAKE_LINKER}/.." REQUIRED)

  add_library(bridge_usb_objects OBJECT bridge_usb.c)
  set_target_properties(bridge_usb_objects PROPERTIES C_STANDARD 11)
  target_compile_options(bridge_usb_objects PRIVATE /std:clatest /W4 /utf-8)
  target_compile_definitions(bridge_usb_objects PRIVATE WIN32_LEAN_AND_MEAN)
  target_include_directories(bridge_usb_objects SYSTEM PRIVATE "${SIGROK_BRIDGE_VCPKG_DIR}/include/libusb-1.0")

  set(sigrok_bridge_shim_def "${CMAKE_CURRENT_BINARY_DIR}/bridge_usb.def")
  add_custom_command(
      OUTPUT "${sigrok_bridge_shim_def}"
      COMMAND "${CMAKE_COMMAND}"
              "-DDUMPBIN=${SIGROK_BRIDGE_DUMPBIN}"
              "-DOBJECT=$<TARGET_OBJECTS:bridge_usb_objects>"
              "-DREAL_DLL=${sigrok_bridge_shim_libusb_dll}"
              "-DOUTPUT=${sigrok_bridge_shim_def}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/make_exports.cmake"
      DEPENDS bridge_usb_objects "${sigrok_bridge_shim_libusb_dll}" make_exports.cmake
      VERBATIM)

  add_library(bridge_usb SHARED $<TARGET_OBJECTS:bridge_usb_objects> "${sigrok_bridge_shim_def}")
  target_link_libraries(bridge_usb PRIVATE "${SIGROK_BRIDGE_VCPKG_DIR}/lib/libusb-1.0.lib")
  set_target_properties(bridge_usb PROPERTIES
      LINKER_LANGUAGE C
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")

  configure_file(bridge_usb.h "${CMAKE_CURRENT_BINARY_DIR}/include/bridge_usb/bridge_usb.h" COPYONLY)
  add_library(bridge_usb_headers INTERFACE)
  target_include_directories(bridge_usb_headers SYSTEM INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
  target_link_libraries(bridge_usb INTERFACE bridge_usb_headers)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SIGROK_BRIDGE_SHIM_LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  add_library(bridge_usb SHARED bridge_usb_linux.c)
  set_target_properties(bridge_usb PROPERTIES
      C_STANDARD 11
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/drivers/lib"
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN")
  target_compile_options(bridge_usb PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(bridge_usb PUBLIC PkgConfig::SIGROK_BRIDGE_SHIM_LIBUSB PRIVATE ${CMAKE_DL_LIBS})
  find_package(Threads REQUIRED)
  target_link_libraries(bridge_usb PRIVATE Threads::Threads)

  configure_file(bridge_usb.h "${CMAKE_CURRENT_BINARY_DIR}/include/bridge_usb/bridge_usb.h" COPYONLY)
  add_library(bridge_usb_headers INTERFACE)
  target_include_directories(bridge_usb_headers SYSTEM INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
  target_link_libraries(bridge_usb INTERFACE bridge_usb_headers)
endif()
