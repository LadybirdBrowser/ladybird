# This source file is part of the Swift open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors.
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information

# Compute the name of the architecture directory on Windows from the CMake
# system processor name.
function(_swift_windows_arch_name output_variable_name target_arch)
  if(NOT WIN32)
    return()
  endif()

  if("${target_arch}" STREQUAL "AMD64")
    set("${output_variable_name}" "x86_64" PARENT_SCOPE)
  elseif("${target_arch}" STREQUAL "ARM64")
    set("${output_variable_name}" "aarch64" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Unknown windows architecture: ${target_arch}")
  endif()
endfunction()

# Compute flags and search paths
# NOTE: This logic will eventually move to CMake
function(_setup_swift_paths)
  # If we haven't set the swift library search paths, do that now
  if(NOT SWIFT_LIBRARY_SEARCH_PATHS OR NOT SWIFT_INCLUDE_PATHS OR (APPLE AND NOT SWIFT_TARGET_TRIPLE))
    if(APPLE)
      set(SDK_FLAGS "-sdk" "${CMAKE_OSX_SYSROOT}")
    endif()

    # Note: This does not handle cross-compiling correctly.
    #       To handle it correctly, we would need to pass the target triple and
    #       flags to this compiler invocation.
    execute_process(
      COMMAND ${CMAKE_Swift_COMPILER} ${SDK_FLAGS} -print-target-info
      OUTPUT_VARIABLE SWIFT_TARGET_INFO
    )

    # FIXME: https://gitlab.kitware.com/cmake/cmake/-/issues/26174
    if (APPLE)
      if (CMAKE_OSX_DEPLOYMENT_TARGET)
        set(SWIFT_TARGET_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE STRING "Swift target triple")
      else()
        string(JSON SWIFT_TARGET_TARGET GET ${SWIFT_TARGET_INFO} "target")
        string(JSON SWIFT_TARGET_TARGET_TRIPLE GET ${SWIFT_TARGET_TARGET} "triple")
        set(SWIFT_TARGET_TRIPLE ${SWIFT_TARGET_TARGET_TRIPLE} CACHE STRING "Swift target triple")
      endif()
    endif()

    # extract search paths from swift driver response
    string(JSON SWIFT_TARGET_PATHS GET ${SWIFT_TARGET_INFO} "paths")

    string(JSON SWIFT_TARGET_LIBRARY_PATHS GET ${SWIFT_TARGET_PATHS} "runtimeLibraryPaths")
    string(JSON SWIFT_TARGET_LIBRARY_PATHS_LENGTH LENGTH ${SWIFT_TARGET_LIBRARY_PATHS})
    math(EXPR SWIFT_TARGET_LIBRARY_PATHS_LENGTH "${SWIFT_TARGET_LIBRARY_PATHS_LENGTH} - 1 ")

    string(JSON SWIFT_TARGET_LIBRARY_IMPORT_PATHS GET ${SWIFT_TARGET_PATHS} "runtimeLibraryImportPaths")
    string(JSON SWIFT_TARGET_LIBRARY_IMPORT_PATHS_LENGTH LENGTH ${SWIFT_TARGET_LIBRARY_IMPORT_PATHS})
    math(EXPR SWIFT_TARGET_LIBRARY_IMPORT_PATHS_LENGTH "${SWIFT_TARGET_LIBRARY_IMPORT_PATHS_LENGTH} - 1 ")

    string(JSON SWIFT_SDK_IMPORT_PATH ERROR_VARIABLE errno GET ${SWIFT_TARGET_PATHS} "sdkPath")

    foreach(JSON_ARG_IDX RANGE ${SWIFT_TARGET_LIBRARY_PATHS_LENGTH})
      string(JSON SWIFT_LIB GET ${SWIFT_TARGET_LIBRARY_PATHS} ${JSON_ARG_IDX})
      list(APPEND SWIFT_SEARCH_PATHS ${SWIFT_LIB})
    endforeach()

    foreach(JSON_ARG_IDX RANGE ${SWIFT_TARGET_LIBRARY_IMPORT_PATHS_LENGTH})
      string(JSON SWIFT_LIB GET ${SWIFT_TARGET_LIBRARY_IMPORT_PATHS} ${JSON_ARG_IDX})
      list(APPEND SWIFT_SEARCH_PATHS ${SWIFT_LIB})
    endforeach()

    if(SWIFT_SDK_IMPORT_PATH)
      list(APPEND SWIFT_SEARCH_PATHS ${SWIFT_SDK_IMPORT_PATH})
    endif()

    # Save the swift library search paths
    set(SWIFT_LIBRARY_SEARCH_PATHS ${SWIFT_SEARCH_PATHS} CACHE FILEPATH "Swift driver search paths")

    string(JSON SWIFT_RUNTIME_RESOURCE_PATH GET ${SWIFT_TARGET_PATHS} "runtimeResourcePath")
    set(SWIFT_TOOLCHAIN_INCLUDE_DIR "${SWIFT_RUNTIME_RESOURCE_PATH}/../../include")
    cmake_path(ABSOLUTE_PATH SWIFT_TOOLCHAIN_INCLUDE_DIR NORMALIZE)
    if (NOT IS_DIRECTORY "${SWIFT_TOOLCHAIN_INCLUDE_DIR}")
	    message(WARNING "Expected toolchain include dir ${SWIFT_TOOLCHAIN_INCLUDE_DIR} does not exist")
    endif()
    set(SWIFT_INCLUDE_PATHS ${SWIFT_TOOLCHAIN_INCLUDE_DIR} CACHE FILEPATH "Swift interop include paths")
  endif()

  link_directories(${SWIFT_LIBRARY_SEARCH_PATHS})
  include_directories(${SWIFT_INCLUDE_PATHS})

  if(WIN32)
    _swift_windows_arch_name(SWIFT_WIN_ARCH_DIR "${CMAKE_SYSTEM_PROCESSOR}")
    set(SWIFT_SWIFTRT_FILE "$ENV{SDKROOT}/usr/lib/swift/windows/${SWIFT_WIN_ARCH_DIR}/swiftrt.obj")
    add_link_options("$<$<LINK_LANGUAGE:Swift>:${SWIFT_SWIFTRT_FILE}>")
  elseif(NOT APPLE)
    find_file(SWIFT_SWIFTRT_FILE
              swiftrt.o
              PATHS ${SWIFT_LIBRARY_SEARCH_PATHS}
              NO_CACHE
              REQUIRED
              NO_DEFAULT_PATH)
    add_link_options("$<$<LINK_LANGUAGE:Swift>:${SWIFT_SWIFTRT_FILE}>")
  endif()
endfunction()

_setup_swift_paths()
