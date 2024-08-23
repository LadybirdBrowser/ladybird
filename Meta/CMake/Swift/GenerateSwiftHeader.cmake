# This source file is part of the Swift open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors.
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information


# Generate the bridging header from Swift to C++
#
# target: the name of the target to generate headers for.
#         This target must build swift source files.
# header: the name of the header file to generate.
#
# NOTE: This logic will eventually be unstreamed into CMake.
function(_swift_generate_cxx_header target header)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Target ${target} not defined.")
  endif()

  if(NOT DEFINED CMAKE_Swift_COMPILER)
    message(WARNING "Swift not enabled in project. Cannot generate headers for Swift files.")
    return()
  endif()

  cmake_parse_arguments(PARSE_ARGV 2 "ARG" "" "MODULE_NAME;CXX_STD_VERSION" "SEARCH_PATHS;COMPILE_OPTIONS")

  if(NOT ARG_MODULE_NAME)
    set(target_module_name $<TARGET_PROPERTY:${target},Swift_MODULE_NAME>)
    set(ARG_MODULE_NAME $<IF:$<BOOL:${target_module_name}>,${target_module_name},${target}>)
  endif()

  if(ARG_SEARCH_PATHS)
    list(TRANSFORM ARG_SEARCH_PATHS PREPEND "-I")
  endif()

  if(APPLE)
    set(SDK_FLAGS "-sdk" "${CMAKE_OSX_SYSROOT}")
    list(APPEND SDK_FLAGS "-target" "${CMAKE_Swift_COMPILER_TARGET}")
  elseif(WIN32)
    set(SDK_FLAGS "-sdk" "$ENV{SDKROOT}")
  elseif(DEFINED ${CMAKE_SYSROOT})
    set(SDK_FLAGS "-sdk" "${CMAKE_SYSROOT}")
    list(APPEND SDK_FLAGS "-target" "${CMAKE_Swift_COMPILER_TARGET}")
  endif()

  cmake_path(APPEND CMAKE_CURRENT_BINARY_DIR include
    OUTPUT_VARIABLE base_path)

  cmake_path(APPEND base_path ${header}
    OUTPUT_VARIABLE header_path)

  if (NOT ARG_CXX_STD_VERSION)
    set(ARG_CXX_STD_VERSION "14")
    get_target_property(CxxFeatures ${target} COMPILE_FEATURES)
    list(FILTER CxxFeatures INCLUDE REGEX "cxx_std_[0-9]+$")
    if (NOT "${CxxFeatures}" STREQUAL "")
        string(SUBSTRING ${CxxFeatures} 8 2 ARG_CXX_STD_VERSION)
    endif()
  endif()

  set(CXX_STD_FLAGS -Xcc -std=c++${ARG_CXX_STD_VERSION})

  set(_AllSources $<TARGET_PROPERTY:${target},SOURCES>)
  set(_SwiftSources $<FILTER:${_AllSources},INCLUDE,\\.swift$>)
  add_custom_command(OUTPUT ${header_path}
    DEPENDS ${_SwiftSources}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND
      ${CMAKE_Swift_COMPILER} -frontend -typecheck
      ${ARG_SEARCH_PATHS}
      ${_SwiftSources}
      ${SDK_FLAGS}
      ${CXX_STD_FLAGS}
      ${ARG_COMPILE_OPTIONS}
      -Xcc -Wno-unqualified-std-cast-call
      -Xcc -Wno-user-defined-literals
      -Xcc -Wno-unknown-warning-option
      -module-name "${ARG_MODULE_NAME}"
      -cxx-interoperability-mode=default
      -emit-clang-header-path ${header_path}
    COMMENT
      "Generating '${header_path}'"
    COMMAND_EXPAND_LISTS)

  # Added to public interface for dependees to find.
  target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${base_path}>)
  # Added to the target to ensure target rebuilds if header changes and is used
  # by sources in the target.
  target_sources(${target} PRIVATE ${header_path})
endfunction()
