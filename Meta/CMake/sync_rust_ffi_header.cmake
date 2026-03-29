#[[
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
]]

if (NOT DEFINED CRATE_NAME OR NOT DEFINED CARGO_BUILD_SCRIPT_DIR OR NOT DEFINED FFI_HEADER OR NOT DEFINED FFI_OUTPUT_DIR)
    message(FATAL_ERROR "sync_rust_ffi_header.cmake requires CRATE_NAME, CARGO_BUILD_SCRIPT_DIR, FFI_HEADER, and FFI_OUTPUT_DIR")
endif()

if (FFI_HEADER STREQUAL "")
    return()
endif()

file(GLOB root_output_files "${CARGO_BUILD_SCRIPT_DIR}/${CRATE_NAME}-*/root-output")

set(latest_source_header "")
set(latest_root_output_timestamp "")

foreach(root_output_file IN LISTS root_output_files)
    file(READ "${root_output_file}" out_dir)
    string(STRIP "${out_dir}" out_dir)

    set(source_header "${out_dir}/${FFI_HEADER}")
    if (NOT EXISTS "${source_header}")
        continue()
    endif()

    file(TIMESTAMP "${root_output_file}" root_output_timestamp "%s" UTC)
    if (latest_source_header STREQUAL "" OR root_output_timestamp GREATER latest_root_output_timestamp)
        set(latest_source_header "${source_header}")
        set(latest_root_output_timestamp "${root_output_timestamp}")
    endif()
endforeach()

if (NOT latest_source_header STREQUAL "")
    file(MAKE_DIRECTORY "${FFI_OUTPUT_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${latest_source_header}" "${FFI_OUTPUT_DIR}/${FFI_HEADER}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    return()
endif()

message(FATAL_ERROR "Failed to find ${FFI_HEADER} in Cargo build output for ${CRATE_NAME}")
