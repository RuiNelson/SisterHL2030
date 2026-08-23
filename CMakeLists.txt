cmake_minimum_required(VERSION 3.20)
# Bump this when shipping a user-visible driver change. The git sha is
# appended automatically so an install can be matched to a exact build.
project(sisterhl2030 VERSION 1.1.4 LANGUAGES CXX)

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

add_library(sisterhl2030_encoder
  src/encoder/job.cc
  src/encoder/line.cc
  src/encoder/halftone.cc
)
target_include_directories(sisterhl2030_encoder PUBLIC src)
target_compile_options(sisterhl2030_encoder PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
)

add_executable(sister-rawtobr src/tools/rawtobr.cpp)
target_link_libraries(sister-rawtobr PRIVATE sisterhl2030_encoder)

add_executable(sister-preview src/tools/preview_halftone.cpp)
target_link_libraries(sister-preview PRIVATE sisterhl2030_encoder)

add_executable(rastertosisterhl2030 src/cups/rastertosisterhl2030.cpp)
target_include_directories(rastertosisterhl2030 PRIVATE src)
target_link_libraries(rastertosisterhl2030 PRIVATE sisterhl2030_encoder cups cupsimage)
target_compile_options(rastertosisterhl2030 PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
)

add_library(sisterhl2030_status src/status/pjl.cc)
target_include_directories(sisterhl2030_status PUBLIC src)
target_compile_options(sisterhl2030_status PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
)

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
endif()

# PAPPL printer application (opt-in while the ippeveprinter façade is still
# the shipping path). PAPPL is not packaged for Homebrew, so we fetch a pinned
# release and build it at configure time -- that way pkg-config can report the
# link line, which carries absolute Homebrew paths and -framework flags.
option(SISTER_WITH_PAPPL "Build the PAPPL printer application" OFF)

if(SISTER_WITH_PAPPL)
  set(PAPPL_VERSION 1.4.12)
  set(PAPPL_SHA256 1684c4e06446e9f7d93a39729fa0ba56f07a4007560080fdad7d0e2076a3615f)
  set(PAPPL_PREFIX ${CMAKE_BINARY_DIR}/pappl)
  set(PAPPL_PC ${PAPPL_PREFIX}/lib/pkgconfig/pappl.pc)

  if(NOT EXISTS ${PAPPL_PC})
    set(_pappl_tar ${CMAKE_BINARY_DIR}/pappl-${PAPPL_VERSION}.tar.gz)
    set(_pappl_src ${CMAKE_BINARY_DIR}/pappl-${PAPPL_VERSION})
    message(STATUS "Fetching PAPPL ${PAPPL_VERSION}")
    file(DOWNLOAD
      https://github.com/michaelrsweet/pappl/releases/download/v${PAPPL_VERSION}/pappl-${PAPPL_VERSION}.tar.gz
      ${_pappl_tar}
      EXPECTED_HASH SHA256=${PAPPL_SHA256}
      SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT ${_pappl_tar} DESTINATION ${CMAKE_BINARY_DIR})
    # 1.4.x builds against the CUPS 2.3 that ships with macOS; 2.x would drag
    # in libcups 3. --disable-shared keeps the app a single binary.
    execute_process(
      COMMAND ${_pappl_src}/configure --prefix=${PAPPL_PREFIX}
              --enable-libusb --disable-shared --with-dnssd=mdnsresponder
      WORKING_DIRECTORY ${_pappl_src}
      RESULT_VARIABLE _pappl_conf
      OUTPUT_FILE ${CMAKE_BINARY_DIR}/pappl-configure.log
      ERROR_FILE ${CMAKE_BINARY_DIR}/pappl-configure.log)
    if(NOT _pappl_conf EQUAL 0)
      message(FATAL_ERROR "PAPPL configure failed, see pappl-configure.log")
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
  endif()

  find_program(PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)
  # Take the flags as one string: pkg-config emits "-framework CoreFoundation",
  # which CMake's list handling splits into two broken arguments.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env PKG_CONFIG_PATH=${PAPPL_PREFIX}/lib/pkgconfig
            ${PKG_CONFIG_EXECUTABLE} --libs pappl
    OUTPUT_VARIABLE PAPPL_LINK_FLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)

  add_executable(sister-printer-app
    src/pappl/sister_app.cpp
    ${SISTER_GENERATED_DIR}/version.h)
  target_include_directories(sister-printer-app PRIVATE src ${PAPPL_PREFIX}/include
                                                         ${SISTER_GENERATED_DIR})
  target_link_libraries(sister-printer-app PRIVATE sisterhl2030_encoder
                                                     sisterhl2030_status)
  set_target_properties(sister-printer-app PROPERTIES LINK_FLAGS "${PAPPL_LINK_FLAGS}")
  target_compile_options(sister-printer-app PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra>
  )
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

# No PPD is installed: the queue is IPP Everywhere, and the installer deletes
# any classic PPD left by earlier versions. ppd/Sister-HL-2030.ppd is kept in
# the tree as a reference for the PJL/PCL options only.
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
