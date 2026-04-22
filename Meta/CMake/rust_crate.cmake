## Store the rust crates attached to a cmake target as crate-name|manifest-path strings.
## This travels with the target through the link graph.
define_property(TARGET PROPERTY TARGET_CRATE_ENTRIES
    BRIEF_DOCS "Rust crates exposed by this target"
    FULL_DOCS "A list of crate-name|manifest-path entries that should be linked into a rust runtime for this target."
)

# Register this crate in TARGET_CRATE_ENTRIES. target_name uses crate_name from manifest_path
function(register_crate target_name crate_name manifest_path)
    if (NOT TARGET "${target_name}")
        message(FATAL_ERROR "Cannot register crate ${crate_name} for unknown target ${target_name}")
    endif()
    if (NOT IS_ABSOLUTE "${manifest_path}")
        set(manifest_path "${CMAKE_CURRENT_SOURCE_DIR}/${manifest_path}")
    endif()

    # Normalize the name-NOTFOUND string for missing targets to an empty list before appending to it.
    get_target_property(target_crates "${target_name}" TARGET_CRATE_ENTRIES)
    if ("${target_crates}" MATCHES "-NOTFOUND$")
        set(target_crates "")
    endif()

    # Dedup here so repeated imports or repeated walks do not grow the list.
    list(APPEND target_crates "${crate_name}|${manifest_path}")
    list(REMOVE_DUPLICATES target_crates)
    set_target_properties("${target_name}" PROPERTIES TARGET_CRATE_ENTRIES "${target_crates}")
endfunction()

# Helper that walks the link graph to collect crates registered to the target and its dependencies.
function(_collect_crates_impl target_name output_var visited_var)
    if (NOT TARGET "${target_name}")
        return()
    endif()
    list(FIND ${visited_var} "${target_name}" target_index)
    if (NOT target_index EQUAL -1)
        return()
    endif()
    list(APPEND ${visited_var} "${target_name}")
    get_target_property(target_crates "${target_name}" TARGET_CRATE_ENTRIES)
    if (NOT "${target_crates}" MATCHES "-NOTFOUND$")
        list(APPEND ${output_var} ${target_crates})
    endif()

    foreach(link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(target_libraries "${target_name}" ${link_property})
        if ("${target_libraries}" MATCHES "-NOTFOUND$")
            continue()
        endif()
        foreach(target_library IN LISTS target_libraries)
            if (TARGET "${target_library}")
                _collect_crates_impl("${target_library}" ${output_var} ${visited_var})
            endif()
        endforeach()
    endforeach()
    set(${output_var} "${${output_var}}" PARENT_SCOPE)
    set(${visited_var} "${${visited_var}}" PARENT_SCOPE)
endfunction()

# Gather crates registered on the target and linked dependencies.
function(collect_crates target_name output_var)
    set(collected_crates "")
    set(visited_targets "")
    _collect_crates_impl("${target_name}" collected_crates visited_targets)
    list(REMOVE_DUPLICATES collected_crates)
    list(SORT collected_crates)
    set(${output_var} "${collected_crates}" PARENT_SCOPE)
endfunction()

# Get crates attached directly to this target. Shared-library builds link each library separately,
# so they only need a local runtime instead of the full transitive closure.
function(collect_local_crates target_name output_var)
    get_target_property(target_crates "${target_name}" TARGET_CRATE_ENTRIES)
    if ("${target_crates}" MATCHES "-NOTFOUND$")
        set(target_crates "")
    endif()
    list(REMOVE_DUPLICATES target_crates)
    list(SORT target_crates)
    set(${output_var} "${target_crates}" PARENT_SCOPE)
endfunction()

# Generate a staticlib crate for a native link target using the passed in crate list (shared lib
# builds) or the collected transitive closure (static builds).
function(link_rust_runtime target_name)
    # Shared library builds pass a direct crate list here so each library gets a private runtime
    # and allocator shim for its own crates.
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "RUST_CRATE_ENTRIES")

    if (ARG_RUST_CRATE_ENTRIES)
        set(target_crates ${ARG_RUST_CRATE_ENTRIES})
    else()
        # Static and final-target callers fall back to the full transitive closure for target_name.
        collect_crates("${target_name}" target_crates)
    endif()
    if (NOT target_crates)
        return()
    endif()

    string(TOLOWER "${target_name}" runtime_name_suffix)
    string(REGEX REPLACE "[^a-z0-9_]" "_" runtime_name_suffix "${runtime_name_suffix}")
    set(runtime_crate_name "ladybird_runtime_${runtime_name_suffix}")
    set(runtime_dir "${CMAKE_BINARY_DIR}/rust-runtime/${target_name}")
    set(runtime_src_dir "${runtime_dir}/src")
    file(MAKE_DIRECTORY "${runtime_src_dir}")
    set(runtime_dependencies "")
    set(runtime_reexports "")
    set(runtime_depends "")
    file(RELATIVE_PATH allocator_path "${runtime_src_dir}" "${LADYBIRD_SOURCE_DIR}/Libraries/LibRustRuntime/src/allocator.rs")

    # Each entry is stored as crate-name|manifest-path. Split it back apart and
    # write the matching cargo dependency line for the generated crate.
    foreach(target_crate IN LISTS target_crates)
        string(REPLACE "|" ";" crate_parts "${target_crate}")
        list(GET crate_parts 0 crate_name)
        list(GET crate_parts 1 crate_manifest_path)
        get_filename_component(crate_dir "${crate_manifest_path}" DIRECTORY)
        file(RELATIVE_PATH crate_path "${runtime_dir}" "${crate_dir}")
        string(APPEND runtime_dependencies "${crate_name} = { path = \"${crate_path}\" }\n")
        string(APPEND runtime_reexports "pub use ${crate_name} as ${crate_name}_runtime_export;\n")
        list(APPEND runtime_depends "${crate_manifest_path}")
    endforeach()

    # Generate the manifest and a lib.rs with the allocator in the build directory.
    file(CONFIGURE OUTPUT "${runtime_dir}/Cargo.toml" CONTENT [=[
[package]
name = "@runtime_crate_name@"
version = "0.1.0"
edition = "2024"
[lib]
crate-type = ["staticlib"]
[workspace]
[dependencies]
@runtime_dependencies@]=] @ONLY)

    file(CONFIGURE OUTPUT "${runtime_src_dir}/lib.rs" CONTENT [=[
#[path = "@allocator_path@"]
mod allocator;
@runtime_reexports@]=] @ONLY)

    # Build the generated crate like any other rust crate, but track the source
    # manifests of the crates it depends on so config changes rebuild it too.
    import_rust_crate(
        MANIFEST_PATH "${runtime_dir}/Cargo.toml"
        CRATE_NAME "${runtime_crate_name}"
        WORKSPACE_DIR "${LADYBIRD_SOURCE_DIR}"
        DEPENDS ${runtime_depends}
    )
    set_property(TARGET "${target_name}" APPEND PROPERTY LINK_LIBRARIES "${runtime_crate_name}")
endfunction()

# import_rust_crate(MANIFEST_PATH path/to/Cargo.toml CRATE_NAME name)
#
# Builds a Rust crate using cargo and creates an IMPORTED target.
# MANIFEST_PATH is relative to CMAKE_CURRENT_SOURCE_DIR unless already absolute.
#
# When corrosion supports dependency tracking, we can use corrosion_import_crate() instead of this function. See:
# https://github.com/corrosion-rs/corrosion/issues/206
# https://github.com/corrosion-rs/corrosion/issues/624
function(import_rust_crate)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "MANIFEST_PATH;CRATE_NAME;CRATE_KIND;FFI_OUTPUT_DIR;FFI_HEADER" "FEATURES")

    if (NOT ARG_CRATE_KIND)
        set(ARG_CRATE_KIND "STATICLIB")
    endif()

    if (NOT IS_ABSOLUTE "${ARG_MANIFEST_PATH}")
        set(ARG_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_MANIFEST_PATH}")
    endif()
    if (NOT ARG_FFI_OUTPUT_DIR)
        set(ARG_FFI_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if (ARG_FFI_HEADER)
        set(ffi_output "${ARG_FFI_OUTPUT_DIR}/${ARG_FFI_HEADER}")
    endif()

    # Find the workspace Cargo.lock to track as a dependency.
    if (ARG_WORKSPACE_DIR)
        set(workspace_dir "${ARG_WORKSPACE_DIR}")
    else()
        get_filename_component(workspace_dir "${ARG_MANIFEST_PATH}" DIRECTORY)
        while(NOT EXISTS "${workspace_dir}/Cargo.lock")
            get_filename_component(workspace_dir "${workspace_dir}" DIRECTORY)
        endwhile()
    endif()

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

    set(cargo_feature_flags "")
    if (ARG_FEATURES)
        list(JOIN ARG_FEATURES "," cargo_features)
        list(APPEND cargo_feature_flags "--features=${cargo_features}")
    endif()

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

    if (ARG_CRATE_KIND STREQUAL "RLIB")
        set(output_lib "${cargo_output_dir}/lib${ARG_CRATE_NAME}.rlib")
        set(depfile "${cargo_output_dir}/lib${ARG_CRATE_NAME}.d")
    elseif (WIN32)
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
                ${cargo_feature_flags}
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
        DEPENDS "${ARG_MANIFEST_PATH}" ${ARG_DEPENDS}
            "${workspace_dir}/Cargo.lock" "${workspace_dir}/Cargo.toml"
        DEPFILE "${depfile}"
        COMMENT "Building Rust crate ${ARG_CRATE_NAME}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
    )

    add_custom_target(${ARG_CRATE_NAME}-build DEPENDS "${output_lib}" ${ffi_output})

    if (ARG_CRATE_KIND STREQUAL "RLIB")
        # The rlib is only an internal rust dependency. c++ uses it for the generated header, not for linking.
        add_library(${ARG_CRATE_NAME} INTERFACE IMPORTED GLOBAL)
        set_target_properties(${ARG_CRATE_NAME} PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${ARG_FFI_OUTPUT_DIR}"
        )
        add_dependencies(${ARG_CRATE_NAME} ${ARG_CRATE_NAME}-build)
        return()
    endif()

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

function(import_rust_ffi_crate target_name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "MANIFEST_PATH;CRATE_NAME;FFI_OUTPUT_DIR;FFI_HEADER" "FEATURES")

    import_rust_crate(
        MANIFEST_PATH "${ARG_MANIFEST_PATH}"
        CRATE_NAME "${ARG_CRATE_NAME}"
        CRATE_KIND RLIB
        FFI_OUTPUT_DIR "${ARG_FFI_OUTPUT_DIR}"
        FFI_HEADER "${ARG_FFI_HEADER}"
        FEATURES ${ARG_FEATURES}
    )

    register_crate("${target_name}" "${ARG_CRATE_NAME}" "${ARG_MANIFEST_PATH}")

    target_link_libraries(${target_name} PRIVATE ${ARG_CRATE_NAME})

    # Shared-library builds want one runtime per library target.
    if (BUILD_SHARED_LIBS)
        collect_local_crates("${target_name}" local_crates)
        link_rust_runtime("${target_name}" RUST_CRATE_ENTRIES ${local_crates})
    endif()
endfunction()
