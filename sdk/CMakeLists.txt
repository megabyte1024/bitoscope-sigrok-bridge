# SPDX-License-Identifier: MIT

include(FetchContent)

FetchContent_Declare(capnproto
    GIT_REPOSITORY https://github.com/capnproto/capnproto.git
    GIT_TAG 373e61ec89e2359f1c362e9b2eadc552f4779306
    # WHY: Add c++/ below with EXCLUDE_FROM_ALL; the bogus SOURCE_SUBDIR keeps FetchContent from adding the repository root.
    SOURCE_SUBDIR no-such-directory
)
FetchContent_MakeAvailable(capnproto)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_TESTING OFF)
# WHY: Set OFF, not AUTO: the build must not pick up an OpenSSL or zlib that happens to be installed.
set(WITH_OPENSSL OFF CACHE STRING "")
set(WITH_ZLIB OFF CACHE STRING "")
add_subdirectory("${capnproto_SOURCE_DIR}/c++" "${capnproto_BINARY_DIR}" EXCLUDE_FROM_ALL)

function(bitoscope_enable_warnings target)
  # WHY: C4324 only reports padding an alignas asks for, GCC and Clang have no such warning (owner, 2026-10-21).
  target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:MSVC>:/W4;/wd4324>
      $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>
  )
endfunction()

add_subdirectory(proto/capnp)
add_subdirectory(proto/driver_capnp)
add_subdirectory(libs/display_text)
add_subdirectory(libs/plugin_ipc)
add_subdirectory(libs/driver_ipc)
add_subdirectory(libs/device_properties)
add_subdirectory(libs/bulk_ring)
add_subdirectory(libs/plugin_ring)
add_subdirectory(libs/driver_ring)
add_subdirectory(libs/plugin_c_sdk)
add_subdirectory(libs/driver_c_sdk)
