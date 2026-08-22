cmake_minimum_required(VERSION 3.20)
project(sisterhl2030 VERSION 0.1.0 LANGUAGES CXX)

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
  )
  target_include_directories(sister-status PRIVATE src)
  target_link_libraries(sister-status PRIVATE sisterhl2030_status
    "-framework IOKit" "-framework CoreFoundation")
  target_compile_options(sister-status PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Wall;-Wextra;-Wpedantic>
  )
endif()

add_executable(test_encoder tests/test_encoder.cpp)
target_link_libraries(test_encoder PRIVATE sisterhl2030_encoder)

add_executable(test_status tests/test_status.cpp)
target_link_libraries(test_status PRIVATE sisterhl2030_status)

enable_testing()
add_test(NAME encoder COMMAND test_encoder)
add_test(NAME status COMMAND test_status)

# No PPD is installed: the queue is IPP Everywhere, and the installer deletes
# any classic PPD left by earlier versions. ppd/Sister-HL-2030.ppd is kept in
# the tree as a reference for the PJL/PCL options only.
set(SISTER_INSTALL_FILTERDIR "/Library/Printers/SisterHL2030/filter")
install(TARGETS rastertosisterhl2030 RUNTIME DESTINATION ${SISTER_INSTALL_FILTERDIR})
if(APPLE)
  install(TARGETS sister-status RUNTIME DESTINATION ${SISTER_INSTALL_FILTERDIR})
endif()
