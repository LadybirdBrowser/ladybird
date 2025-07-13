# Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
# Copyright (c) 2023, Daniel Bertalan <dani@danielbertalan.dev>
#
# SPDX-License-Identifier: BSD-2-Clause
#

if (NOT APPLE AND NOT ANDROID AND NOT VCPKG_TARGET_ANDROID AND NOT WIN32 AND NOT LAGOM_USE_LINKER)
    find_program(LLD_LINKER NAMES "ld.lld")
    if (LLD_LINKER)
        message(STATUS "Using LLD to link Lagom.")
        set(LAGOM_USE_LINKER "lld" CACHE STRING "" FORCE)
    else()
        find_program(MOLD_LINKER NAMES "ld.mold")
        if (MOLD_LINKER)
            message(STATUS "Using mold to link Lagom.")
            set(LAGOM_USE_LINKER "mold" CACHE STRING "" FORCE)
        endif()
    endif()
endif()

if(WIN32 AND NOT LAGOM_USE_LINKER)
    # We do not need to check for its presence.
    # We know it is there with the installation of clang-cl which we require.
    set(LAGOM_USE_LINKER "lld" CACHE STRING "" FORCE)
endif()

if (LAGOM_USE_LINKER)
    # FIXME: Move to only setting CMAKE_LINKER_TYPE once we drop support for CMake < 3.29
    # NOTE: We can't use CMAKE_SYSTEM_NAME because it's not set before the first project call
    # FIXME: https://gitlab.kitware.com/cmake/cmake/-/issues/27037
    if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.29 AND NOT CMAKE_HOST_SYSTEM_NAME MATCHES "FreeBSD")
        string(TOUPPER ${LAGOM_USE_LINKER} linker_type)
        set(CMAKE_LINKER_TYPE ${linker_type})
    else()
        set(LINKER_FLAG "-fuse-ld=${LAGOM_USE_LINKER}")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINKER_FLAG}")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINKER_FLAG}")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LINKER_FLAG}")
    endif()
endif()

if (LAGOM_LINK_POOL_SIZE)
    set_property(GLOBAL PROPERTY JOB_POOLS link_pool=${LAGOM_LINK_POOL_SIZE})
    set(CMAKE_JOB_POOL_LINK link_pool CACHE STRING "Linking job pool")
endif()
