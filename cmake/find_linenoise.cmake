# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2022 Rong Tao
#
# - Try to find liblinenoise
# Once done this will define
#
#  LIBLINENOISE_FOUND - system has liblinenoise
#  LIBLINENOISE_INCLUDE_DIRS - the liblinenoise include directory
#  LIBLINENOISE_LIBRARIES - Link these to use liblinenoise
#  LIBLINENOISE_DEFINITIONS - Compiler switches required for using liblinenoise
#  HAVE_LINENOISE_SET_MULTI_LINE - API linenoiseSetMultiLine()

find_path(LIBLINENOISE_INCLUDE_DIRS
  NAMES
    linenoise.h
  PATHS
    ENV CPATH)

find_library(LIBLINENOISE_LIBRARIES
  NAMES
    linenoise
  PATHS
    ENV LIBRARY_PATH
    ENV LD_LIBRARY_PATH)

include(FindPackageHandleStandardArgs)

FIND_PACKAGE_HANDLE_STANDARD_ARGS(LibLinenoise "Please install the linenoise development package"
  LIBLINENOISE_LIBRARIES
  LIBLINENOISE_INCLUDE_DIRS)

mark_as_advanced(LIBLINENOISE_LIBRARIES LIBLINENOISE_INCLUDE_DIRS)

#message(STATUS "LIBLINENOISE_INCLUDE_DIRS: ${LIBLINENOISE_INCLUDE_DIRS}")
#message(STATUS "LIBLINENOISE_LIBRARIES: ${LIBLINENOISE_LIBRARIES}")

SET(CMAKE_REQUIRED_LIBRARIES)
SET(CMAKE_REQUIRED_INCLUDES)

INCLUDE(CheckCSourceCompiles)
SET(CMAKE_REQUIRED_LIBRARIES ${LIBLINENOISE_LIBRARIES})
SET(CMAKE_REQUIRED_INCLUDES ${LIBLINENOISE_INCLUDE_DIRS})
CHECK_C_SOURCE_COMPILES("
#include <stdio.h>
#include <linenoise.h>
int main() {
	linenoiseSetMultiLine(1);
	return 0;
}" HAVE_LINENOISE)
SET(CMAKE_REQUIRED_LIBRARIES)

mark_as_advanced(LIBLINENOISE_LIBRARIES LIBLINENOISE_INCLUDE_DIRS HAVE_LINENOISE)
