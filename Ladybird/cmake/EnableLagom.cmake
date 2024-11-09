# Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
#
# SPDX-License-Identifier: MIT

set(LAGOM_SOURCE_DIR "${LADYBIRD_SOURCE_DIR}/Meta/Lagom")
set(LAGOM_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/Lagom")

# FIXME: Setting target_include_directories on Lagom libraries might make this unnecessary?
include_directories(${LADYBIRD_SOURCE_DIR})
include_directories(${LADYBIRD_SOURCE_DIR}/Services)
include_directories(${LADYBIRD_SOURCE_DIR}/Userland/Libraries)
include_directories(${LAGOM_BINARY_DIR})
include_directories(${LAGOM_BINARY_DIR}/Services)
include_directories(${LAGOM_BINARY_DIR}/Userland/Libraries)

add_subdirectory("${LAGOM_SOURCE_DIR}" "${LAGOM_BINARY_DIR}")
