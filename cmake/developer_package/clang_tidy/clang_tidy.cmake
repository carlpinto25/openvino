# Copyright (C) 2018-2024 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

if(ENABLE_CLANG_TIDY)
    set(CLANG_TIDY_REQUIRED_VERSION 18 CACHE STRING "clang-tidy version to use")
    set(CLANG_TIDY_FILENAME clang-tidy-${CLANG_TIDY_REQUIRED_VERSION} clang-tidy)
    find_host_program(CLANG_TIDY NAMES ${CLANG_TIDY_FILENAME} PATHS ENV PATH)
    if(CLANG_TIDY)
        execute_process(COMMAND ${CLANG_TIDY} ${CMAKE_CURRENT_SOURCE_DIR} ARGS --version OUTPUT_VARIABLE CLANG_VERSION)
        if(NOT CLANG_VERSION)
            message(FATAL_ERROR "Supported clang-tidy version is ${CLANG_TIDY_REQUIRED_VERSION}!")
        else()
            string(REGEX REPLACE "[^0-9]+([0-9]+)\\..*" "\\1" CLANG_TIDY_MAJOR_VERSION ${CLANG_VERSION})
            if(NOT CLANG_TIDY_MAJOR_VERSION EQUAL CLANG_TIDY_REQUIRED_VERSION)
                message(FATAL_ERROR "Supported clang-tidy version is ${CLANG_TIDY_REQUIRED_VERSION}! Provided version ${CLANG_TIDY_MAJOR_VERSION}")
            endif()
        endif()
    else()
        message(FATAL_ERROR "Supported clang-tidy-${CLANG_TIDY_REQUIRED_VERSION} is not found!")
    endif()
endif()
