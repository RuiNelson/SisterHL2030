cmake_minimum_required(VERSION 3.20)
# Bump this when shipping a user-visible driver change. The git sha is
# appended automatically so an install can be matched to a exact build.
project(sisterhl2030 VERSION 0.9.1 LANGUAGES CXX)

set(SISTER_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(_sister_git_head "${CMAKE_SOURCE_DIR}/.git/HEAD")
set(_sister_ver_deps
  "${CMAKE_SOURCE_DIR}/cmake/write_version.cmake"
  "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
if(EXISTS "${_sister_git_head}")
  list(APPEND _sister_ver_deps "${_sister_git_head}")
endif()
add_custom_command(
  OUTPUT "${SISTER_GENERATED_DIR}/version.h"
  COMMAND ${CMAKE_COMMAND}
    -DSISTER_VERSION=${PROJECT_VERSION}
    -DSISTER_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DSISTER_OUTPUT=${SISTER_GENERATED_DIR}/version.h
    -P ${CMAKE_SOURCE_DIR}/cmake/write_version.cmake
  DEPENDS ${_sister_ver_deps}
  COMMENT "Writing driver version header"
)
# So the first compile after a fresh configure does not race the header.
execute_process(
  COMMAND ${CMAKE_COMMAND}
    -DSISTER_VERSION=${PROJECT_VERSION}
    -DSISTER_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DSISTER_OUTPUT=${SISTER_GENERATED_DIR}/version.h
    -P ${CMAKE_SOURCE_DIR}/cmake/write_version.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures" FORCE)
endif()

# Smallest shipping code. CMake's Release default is -O3 (speed, larger
# __text). Atkinson's extra RAM during a job is transient and fine.
string(REPLACE "-O3" "-Os" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING
    "Flags used by the CXX compiler during RELEASE builds." FORCE)

function(sister_compile_small tgt)
  target_compile_options(${tgt} PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-fno-exceptions;-fno-rtti>)
endfunction()

function(sister_strip tgt)
  if(NOT CMAKE_STRIP)
    return()
  endif()
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_STRIP} "$<TARGET_FILE:${tgt}>"
    COMMENT "Stripping ${tgt}")
endfunction()

add_library(sisterhl2030_encoder
  src/encoder/job.cc
  src/encoder/line.cc
  src/encoder/halftone.cc
)
target_include_directories(sisterhl2030_encoder PUBLIC src)
target_compile_options(sisterhl2030_encoder PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
)
sister_compile_small(sisterhl2030_encoder)

add_executable(sister-rawtobr src/tools/rawtobr.cpp)
target_link_libraries(sister-rawtobr PRIVATE sisterhl2030_encoder)
sister_compile_small(sister-rawtobr)
sister_strip(sister-rawtobr)

add_executable(sister-preview src/tools/preview_halftone.cpp)
target_link_libraries(sister-preview PRIVATE sisterhl2030_encoder)
sister_compile_small(sister-preview)
sister_strip(sister-preview)

add_executable(rastertosisterhl2030 src/cups/rastertosisterhl2030.cpp)
target_include_directories(rastertosisterhl2030 PRIVATE src)
target_link_libraries(rastertosisterhl2030 PRIVATE sisterhl2030_encoder cups cupsimage)
target_compile_options(rastertosisterhl2030 PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
)
sister_compile_small(rastertosisterhl2030)
sister_strip(rastertosisterhl2030)

add_library(sisterhl2030_status src/status/pjl.cc)
target_include_directories(sisterhl2030_status PUBLIC src)
target_compile_options(sisterhl2030_status PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
)
sister_compile_small(sisterhl2030_status)

if(APPLE)
  add_executable(sister-status
    src/tools/sister_status.cpp
    src/status/usb_printer.cc
    ${SISTER_GENERATED_DIR}/version.h
  )
  target_include_directories(sister-status PRIVATE src ${SISTER_GENERATED_DIR})
  target_link_libraries(sister-status PRIVATE sisterhl2030_status
    "-framework IOKit" "-framework CoreFoundation")
  target_compile_options(sister-status PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
  )
  sister_compile_small(sister-status)
  sister_strip(sister-status)
endif()

# PAPPL printer application (the shipping driver). PAPPL is not packaged for
# Homebrew, so we fetch a pinned release and build it at configure time -- that
# way pkg-config can report the link line, which carries absolute Homebrew
# paths and -framework flags.
option(SISTER_WITH_PAPPL "Build the PAPPL printer application" OFF)

if(SISTER_WITH_PAPPL)
  set(PAPPL_VERSION 1.4.12)
  set(PAPPL_SHA256 1684c4e06446e9f7d93a39729fa0ba56f07a4007560080fdad7d0e2076a3615f)
  set(PAPPL_PREFIX ${CMAKE_BINARY_DIR}/pappl)
  set(PAPPL_PC ${PAPPL_PREFIX}/lib/pkgconfig/pappl.pc)
  # Bump when cmake/patch_pappl_idle.cmake or the configure line changes so
  # the next cmake run rebuilds libpappl instead of reusing a fat/AppKit build.
  set(PAPPL_STAMP_VALUE "text-3")
  set(PAPPL_STAMP ${PAPPL_PREFIX}/.sister-pappl-stamp)

  set(_pappl_stamp_now "")
  if(EXISTS ${PAPPL_STAMP})
    file(READ ${PAPPL_STAMP} _pappl_stamp_now)
    string(STRIP "${_pappl_stamp_now}" _pappl_stamp_now)
  endif()

  if(NOT _pappl_stamp_now STREQUAL PAPPL_STAMP_VALUE)
    set(_pappl_tar ${CMAKE_BINARY_DIR}/pappl-${PAPPL_VERSION}.tar.gz)
    set(_pappl_src ${CMAKE_BINARY_DIR}/pappl-${PAPPL_VERSION})
    if(NOT EXISTS ${_pappl_tar})
      message(STATUS "Fetching PAPPL ${PAPPL_VERSION}")
      file(DOWNLOAD
        https://github.com/michaelrsweet/pappl/releases/download/v${PAPPL_VERSION}/pappl-${PAPPL_VERSION}.tar.gz
        ${_pappl_tar}
        EXPECTED_HASH SHA256=${PAPPL_SHA256}
        SHOW_PROGRESS)
    endif()
    # Re-extract so the idle patches land on a clean tree, not a previously
    # patched copy.
    file(REMOVE_RECURSE ${_pappl_src} ${PAPPL_PREFIX})
    file(ARCHIVE_EXTRACT INPUT ${_pappl_tar} DESTINATION ${CMAKE_BINARY_DIR})
    execute_process(
      COMMAND ${CMAKE_COMMAND}
              -DPAPPL_SRC=${_pappl_src}
              -P ${CMAKE_SOURCE_DIR}/cmake/patch_pappl_idle.cmake
      RESULT_VARIABLE _pappl_patch)
    if(NOT _pappl_patch EQUAL 0)
      message(FATAL_ERROR "PAPPL idle patch failed")
    endif()
    # 1.4.x builds against the CUPS 2.3 that ships with macOS; 2.x would drag
    # in libcups 3. --disable-shared keeps the app a single binary.
    execute_process(
      COMMAND ${_pappl_src}/configure --prefix=${PAPPL_PREFIX}
              --enable-libusb --disable-libjpeg --disable-shared
              --with-dnssd=mdnsresponder
      WORKING_DIRECTORY ${_pappl_src}
      RESULT_VARIABLE _pappl_conf
      OUTPUT_FILE ${CMAKE_BINARY_DIR}/pappl-configure.log
      ERROR_FILE ${CMAKE_BINARY_DIR}/pappl-configure.log)
    if(NOT _pappl_conf EQUAL 0)
      message(FATAL_ERROR "PAPPL configure failed, see pappl-configure.log")
    endif()
    execute_process(
      COMMAND ${CMAKE_COMMAND}
              -DPAPPL_SRC=${_pappl_src}
              -DPAPPL_MAKEDEFS=${_pappl_src}/Makedefs
              -P ${CMAKE_SOURCE_DIR}/cmake/patch_pappl_idle.cmake
      RESULT_VARIABLE _pappl_mkpatch)
    if(NOT _pappl_mkpatch EQUAL 0)
      message(FATAL_ERROR "PAPPL Makedefs patch failed")
    endif()
    execute_process(
      COMMAND make -j install
      WORKING_DIRECTORY ${_pappl_src}
      RESULT_VARIABLE _pappl_make
      OUTPUT_FILE ${CMAKE_BINARY_DIR}/pappl-build.log
      ERROR_FILE ${CMAKE_BINARY_DIR}/pappl-build.log)
    if(NOT _pappl_make EQUAL 0)
      message(FATAL_ERROR "PAPPL build failed, see pappl-build.log")
    endif()
    # configure still writes AppKit into pappl.pc; drop it so the app does not
    # link a UI framework the dummy status object never references.
    file(READ ${PAPPL_PC} _pappl_pc)
    string(REPLACE " -framework AppKit" "" _pappl_pc "${_pappl_pc}")
    file(WRITE ${PAPPL_PC} "${_pappl_pc}")
    file(WRITE ${PAPPL_STAMP} "${PAPPL_STAMP_VALUE}\n")
  endif()

  find_program(PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)
  # Take the flags as one string: pkg-config emits "-framework CoreFoundation",
  # which CMake's list handling splits into two broken arguments.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${PAPPL_PREFIX}/lib/pkgconfig
            ${PKG_CONFIG_EXECUTABLE} --libs pappl
    OUTPUT_VARIABLE PAPPL_LINK_FLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  string(REPLACE "-framework AppKit" "" PAPPL_LINK_FLAGS "${PAPPL_LINK_FLAGS}")
  string(STRIP "${PAPPL_LINK_FLAGS}" PAPPL_LINK_FLAGS)

  add_executable(sister-printer-app
    src/pappl/sister_app.cpp
    ${SISTER_GENERATED_DIR}/version.h)
  target_include_directories(sister-printer-app PRIVATE src ${PAPPL_PREFIX}/include
                                                         ${SISTER_GENERATED_DIR})
  target_link_libraries(sister-printer-app PRIVATE sisterhl2030_encoder
                                                     sisterhl2030_status)
  set_target_properties(sister-printer-app PROPERTIES
    LINK_FLAGS "${PAPPL_LINK_FLAGS} -Wl,-dead_strip -Wl,-x")
  target_compile_options(sister-printer-app PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra>
  )
  sister_compile_small(sister-printer-app)
  sister_strip(sister-printer-app)
endif()

add_executable(test_encoder tests/test_encoder.cpp)
target_link_libraries(test_encoder PRIVATE sisterhl2030_encoder)

add_executable(test_status tests/test_status.cpp)
target_link_libraries(test_status PRIVATE sisterhl2030_status)

enable_testing()
add_test(NAME encoder COMMAND test_encoder)
add_test(NAME status COMMAND test_status)
if(SISTER_WITH_PAPPL)
  add_test(NAME pappl_features
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/test_pappl_features.py)
  set_tests_properties(pappl_features PROPERTIES TIMEOUT 120)
  add_test(NAME driver_version
    COMMAND ${CMAKE_BINARY_DIR}/sister-printer-app --version)
  set_tests_properties(driver_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^${PROJECT_VERSION}\\+")
endif()

# No classic PPD is shipped: the queue is created from Apple's ipp2ppd, and
# the installer deletes any Sister PPD left by earlier versions. Advertised
# attributes live in driver_cb() in src/pappl/sister_app.cpp.
# What an install consists of: the printer application and the status CLI.
# rastertosisterhl2030 is deliberately not installed -- the classic CUPS filter
# path is retired, and the binary is kept only as an encoder reference.
set(SISTER_INSTALL_DIR "/Library/Printers/SisterHL2030")
if(APPLE)
  install(TARGETS sister-status RUNTIME DESTINATION ${SISTER_INSTALL_DIR})
endif()
if(SISTER_WITH_PAPPL)
  install(TARGETS sister-printer-app RUNTIME DESTINATION ${SISTER_INSTALL_DIR})
endif()
