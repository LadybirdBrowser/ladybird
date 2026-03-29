# import_rust_crate(MANIFEST_PATH path/to/Cargo.toml CRATE_NAME name)
#
# Builds a Rust static library crate using cargo and creates an IMPORTED target.
# MANIFEST_PATH is relative to CMAKE_CURRENT_SOURCE_DIR.
#
# When corrosion supports dependency tracking, we can use corrosion_import_crate() instead of this function. See:
# https://github.com/corrosion-rs/corrosion/issues/206
# https://github.com/corrosion-rs/corrosion/issues/624
function(import_rust_crate)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "MANIFEST_PATH;CRATE_NAME;FFI_OUTPUT_DIR;FFI_HEADER" "")

    set(ARG_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_MANIFEST_PATH}")
    if (NOT ARG_FFI_OUTPUT_DIR)
        set(ARG_FFI_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if (ARG_FFI_HEADER)
        set(ffi_output "${ARG_FFI_OUTPUT_DIR}/${ARG_FFI_HEADER}")
    endif()

    # Find the workspace Cargo.lock to track as a dependency.
    get_filename_component(workspace_dir "${ARG_MANIFEST_PATH}" DIRECTORY)
    while(NOT EXISTS "${workspace_dir}/Cargo.lock")
        get_filename_component(workspace_dir "${workspace_dir}" DIRECTORY)
    endwhile()

    # Detect the Rust toolchain.
    find_program(RUST_CARGO cargo REQUIRED)
    find_program(RUST_RUSTC rustc REQUIRED)
    if (NOT DEFINED CACHE{RUST_TARGET_TRIPLE})
        execute_process(COMMAND "${RUST_RUSTC}" -vV OUTPUT_VARIABLE rustc_verbose)
        string(REGEX MATCH "host: ([^\n]+)" _ "${rustc_verbose}")
        string(STRIP "${CMAKE_MATCH_1}" host_triple)
        set(RUST_TARGET_TRIPLE "${host_triple}" CACHE INTERNAL "Rust target triple")
    endif()

    # Build the uppercased and underscored variants of the target triple.
    string(REPLACE "-" "_" target_underscore "${RUST_TARGET_TRIPLE}")
    string(TOUPPER "${target_underscore}" target_upper)

    # Determine the cargo profile and output directory name.
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
    if (build_type_upper STREQUAL "DEBUG")
        set(cargo_profile_flag "")
        set(cargo_profile_dir "debug")
    else()
        set(cargo_profile_flag "--release")
        set(cargo_profile_dir "release")
    endif()

    set(cargo_target_dir "${CMAKE_BINARY_DIR}/cargo/build")
    set(cargo_output_dir "${cargo_target_dir}/${RUST_TARGET_TRIPLE}/${cargo_profile_dir}")

    if (WIN32)
        set(output_lib "${cargo_output_dir}/${ARG_CRATE_NAME}.lib")
        set(depfile "${cargo_output_dir}/${ARG_CRATE_NAME}.d")
    else()
        set(output_lib "${cargo_output_dir}/lib${ARG_CRATE_NAME}.a")
        set(depfile "${cargo_output_dir}/lib${ARG_CRATE_NAME}.d")
    endif()

    # Build environment variables for cargo.
    set(cargo_env
        "CC_${target_underscore}=${CMAKE_C_COMPILER}"
        "CXX_${target_underscore}=${CMAKE_CXX_COMPILER}"
        "CARGO_BUILD_RUSTC=${RUST_RUSTC}"
        "FFI_OUTPUT_DIR=${ARG_FFI_OUTPUT_DIR}"
    )

    # On Windows, rustc invokes the linker directly with MSVC-style flags, so we must not override it with a
    # compiler driver like clang-cl.
    if (NOT WIN32)
        list(APPEND cargo_env
            "CARGO_TARGET_${target_upper}_LINKER=${CMAKE_C_COMPILER}"
            "AR_${target_underscore}=${CMAKE_AR}"
        )
    endif()

    if (APPLE AND CMAKE_OSX_SYSROOT)
        list(APPEND cargo_env "SDKROOT=${CMAKE_OSX_SYSROOT}")
    endif()

    add_custom_command(
        OUTPUT "${output_lib}" ${ffi_output}
        COMMAND
            ${CMAKE_COMMAND} -E env ${cargo_env}
            "${RUST_CARGO}"
                rustc
                --lib
                "--target=${RUST_TARGET_TRIPLE}"
                --package ${ARG_CRATE_NAME}
                --manifest-path "${ARG_MANIFEST_PATH}"
                --target-dir "${cargo_target_dir}"
                ${cargo_profile_flag}
                --
                -Cdefault-linker-libraries=yes
                --emit=dep-info
        COMMAND
            ${CMAKE_COMMAND}
                -DCARGO_BUILD_SCRIPT_DIR=${cargo_output_dir}/build
                -DCRATE_NAME=${ARG_CRATE_NAME}
                -DFFI_HEADER=${ARG_FFI_HEADER}
                -DFFI_OUTPUT_DIR=${ARG_FFI_OUTPUT_DIR}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sync_rust_ffi_header.cmake"
        DEPENDS "${ARG_MANIFEST_PATH}"
            "${workspace_dir}/Cargo.lock" "${workspace_dir}/Cargo.toml"
        DEPFILE "${depfile}"
        COMMENT "Building Rust crate ${ARG_CRATE_NAME}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
    )

    add_custom_target(${ARG_CRATE_NAME}-build DEPENDS "${output_lib}" ${ffi_output})

    add_library(${ARG_CRATE_NAME} STATIC IMPORTED GLOBAL)
    set_target_properties(${ARG_CRATE_NAME} PROPERTIES
            IMPORTED_LOCATION "${output_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_FFI_OUTPUT_DIR}"
    )
    add_dependencies(${ARG_CRATE_NAME} ${ARG_CRATE_NAME}-build)

    # Rust staticlibs bundle the standard library, which on Windows depends on system libraries.
    if (WIN32)
        set_target_properties(${ARG_CRATE_NAME} PROPERTIES
            INTERFACE_LINK_LIBRARIES "kernel32;ntdll;Ws2_32;userenv"
        )
    endif()
endfunction()
