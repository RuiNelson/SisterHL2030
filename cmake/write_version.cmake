# Writes generated/version.h. Invoked at configure and before each build.
if(NOT SISTER_VERSION)
  set(SISTER_VERSION "0.0.0")
endif()
if(NOT SISTER_SOURCE_DIR)
  set(SISTER_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
endif()
if(NOT SISTER_OUTPUT)
  message(FATAL_ERROR "SISTER_OUTPUT is not set")
endif()

set(SISTER_GIT_SHA "unknown")
set(SISTER_GIT_DIRTY "")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SISTER_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE SISTER_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT SISTER_GIT_SHA)
    set(SISTER_GIT_SHA "unknown")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SISTER_SOURCE_DIR}" status --porcelain
    OUTPUT_VARIABLE _dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_dirty)
    set(SISTER_GIT_DIRTY "-dirty")
  endif()
endif()

set(SISTER_VERSION_FULL "${SISTER_VERSION}+${SISTER_GIT_SHA}${SISTER_GIT_DIRTY}")

get_filename_component(_dir "${SISTER_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(WRITE "${SISTER_OUTPUT}"
"#ifndef SISTERHL2030_VERSION_H
#define SISTERHL2030_VERSION_H

#define SISTER_VERSION \"${SISTER_VERSION}\"
#define SISTER_GIT_SHA \"${SISTER_GIT_SHA}${SISTER_GIT_DIRTY}\"
#define SISTER_VERSION_FULL \"${SISTER_VERSION_FULL}\"

#endif
")
message(STATUS "SisterHL2030 ${SISTER_VERSION_FULL}")
