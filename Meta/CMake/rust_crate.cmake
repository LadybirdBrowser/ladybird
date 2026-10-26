set_property(GLOBAL PROPERTY JOB_POOLS "${JOB_POOLS};cargo=1")

# Compiler flags shared by Rust libraries, binaries, and tests.
set(RUSTC_TARGET_FLAGS -Dwarnings ${RUSTC_TARGET_CPU_FLAGS} ${RUSTC_TARGET_FRAME_POINTER_FLAGS})
list(JOIN RUSTC_TARGET_FLAGS " " RUSTC_TARGET_FLAGS)

# import_rust_crate(MANIFEST_PATH path/to/Cargo.toml CRATE_NAME name [PANIC_UNWIND] [NO_DEFAULT_FEATURES]
#                   [FEATURES feature...] [KEEP_SYMBOLS symbol...] [ENVIRONMENT VARIABLE=value...])
#
# Builds a Rust static library crate using cargo and creates an IMPORTED target.
# MANIFEST_PATH is relative to CMAKE_CURRENT_SOURCE_DIR.
# NO_DEFAULT_FEATURES builds the crate with only the FEATURES given, without its default ones.
# ENVIRONMENT sets variables in the environment of cargo, for the build script of the crate to read.
# KEEP_SYMBOLS names entry points that only dlsym() reaches. A link only takes the archive members
# something references, and a debug build gives a lone function a member of its own, so these have
# to be asked for or they are left out.
#
# When corrosion supports dependency tracking, we can use corrosion_import_crate() instead of this function. See:
# https://github.com/corrosion-rs/corrosion/issues/206
# https://github.com/corrosion-rs/corrosion/issues/624
function(import_rust_crate)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "PANIC_UNWIND;NO_DEFAULT_FEATURES" "MANIFEST_PATH;CRATE_NAME;FFI_OUTPUT_DIR;FFI_HEADER" "FEATURES;FFI_HEADERS;KEEP_SYMBOLS;ENVIRONMENT")

    if (NOT ARG_FFI_OUTPUT_DIR)
        set(ARG_FFI_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    set(ffi_headers ${ARG_FFI_HEADERS})
    if (ARG_FFI_HEADER)
        list(APPEND ffi_headers "${ARG_FFI_HEADER}")
    endif()

    _rust_crate_common_setup(
        MANIFEST_PATH "${ARG_MANIFEST_PATH}"
        CRATE_NAME ${ARG_CRATE_NAME}
        FFI_OUTPUT_DIR "${ARG_FFI_OUTPUT_DIR}"
    )

    # Rust's release profile normally aborts on panic. Crates that parse untrusted input may opt
    # into unwinding so they can contain dependency panics at their FFI boundary.
    if (ARG_PANIC_UNWIND)
        list(APPEND cargo_env "CARGO_PROFILE_RELEASE_PANIC=unwind")
    endif()
    list(APPEND cargo_env ${ARG_ENVIRONMENT})

    set(cargo_feature_flags "")
    if (ARG_NO_DEFAULT_FEATURES)
        list(APPEND cargo_feature_flags "--no-default-features")
    endif()
    if (ARG_FEATURES)
        list(JOIN ARG_FEATURES "," cargo_features)
        list(APPEND cargo_feature_flags "--features=${cargo_features}")
    endif()

    if (WIN32)
        set(output_lib "${cargo_output_dir}/${ARG_CRATE_NAME}.lib")
        set(depfile "${cargo_output_dir}/${ARG_CRATE_NAME}.d")
    else()
        set(output_lib "${cargo_output_dir}/lib${ARG_CRATE_NAME}.a")
        set(depfile "${cargo_output_dir}/lib${ARG_CRATE_NAME}.d")
    endif()

    _rust_crate_build_command(OUTPUT "${output_lib}" DEPFILE "${depfile}")

    set(ffi_output "")
    if (ffi_headers)
        _rust_crate_sync_ffi_headers(
            CRATE_NAME ${ARG_CRATE_NAME}
            CARGO_OUTPUT "${output_lib}"
            FFI_OUTPUT_DIR "${ARG_FFI_OUTPUT_DIR}"
            FFI_HEADERS ${ffi_headers}
            OUTPUT_VAR ffi_output
        )
    endif()

    add_custom_target(${ARG_CRATE_NAME}-build DEPENDS "${output_lib}" ${ffi_output})

    add_library(${ARG_CRATE_NAME} STATIC IMPORTED GLOBAL)
    set_target_properties(${ARG_CRATE_NAME} PROPERTIES
            IMPORTED_LOCATION "${output_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${ARG_FFI_OUTPUT_DIR}"
    )
    add_dependencies(${ARG_CRATE_NAME} ${ARG_CRATE_NAME}-build)

    foreach(symbol IN LISTS ARG_KEEP_SYMBOLS)
        if (APPLE)
            target_link_options(${ARG_CRATE_NAME} INTERFACE "LINKER:-u,_${symbol}")
        elseif (WIN32)
            target_link_options(${ARG_CRATE_NAME} INTERFACE "LINKER:/INCLUDE:${symbol}")
        else()
            target_link_options(${ARG_CRATE_NAME} INTERFACE "LINKER:-u,${symbol}")
        endif()
    endforeach()

    add_rust_crate_initializer(${ARG_CRATE_NAME})
    target_link_libraries(${ARG_CRATE_NAME} INTERFACE $<TARGET_OBJECTS:${ARG_CRATE_NAME}_init>)

    _rust_crate_link_runtime_libraries(${ARG_CRATE_NAME})

    # The archive on its own, without the initializer and the kept symbols of the library that imports the crate, for a
    # target that takes a crate the archive bundles out of it.
    add_library(${ARG_CRATE_NAME}_archive STATIC IMPORTED GLOBAL)
    set_target_properties(${ARG_CRATE_NAME}_archive PROPERTIES IMPORTED_LOCATION "${output_lib}")
    add_dependencies(${ARG_CRATE_NAME}_archive ${ARG_CRATE_NAME}-build)
    _rust_crate_link_runtime_libraries(${ARG_CRATE_NAME}_archive)
endfunction()

# The cargo invocation that builds the library of the crate _rust_crate_common_setup() was called for, as the custom
# command that produces OUTPUT with the dependencies cargo lists in DEPFILE.
function(_rust_crate_build_command)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "OUTPUT;DEPFILE" "")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
            -DLOG=${cargo_output_dir}/cargo-${ARG_CRATE_NAME}.log
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_quiet.cmake"
            --
            ${CMAKE_COMMAND} -E env ${cargo_env} CARGO_TERM_PROGRESS_WHEN=never TERM=dumb
            "${RUST_CARGO}"
                rustc
                --lib
                ${cargo_feature_flags}
                ${cargo_common_flags}
        DEPENDS "${manifest_path}"
            "${workspace_dir}/Cargo.lock" "${workspace_dir}/Cargo.toml"
            "${RUST_RUSTC}" "${CMAKE_SOURCE_DIR}/rust-toolchain.toml"
        DEPFILE "${ARG_DEPFILE}"
        COMMENT "Building Rust crate ${ARG_CRATE_NAME}"
        JOB_POOL cargo
        COMMAND_EXPAND_LISTS
    )
endfunction()

# add_rust_crate_initializer(crate)
#
# Defines the object library <crate>_init, whose one object sets up the panic hook and the demangler of the crate in
# whatever links the object. import_rust_crate() defines it for the crate it imports; a target that takes a crate out
# of the archive of another import defines it for that crate.
function(add_rust_crate_initializer crate)
    set(RUST_CRATE_NAME ${crate})
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RustCrateInit.cpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${crate}_init.cpp" @ONLY)

    # A static library only links in the objects that something references, and nothing references the initializer.
    # Putting its object on the link line runs it in whatever executable or shared library links the crate.
    add_library(${crate}_init OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${crate}_init.cpp")
endfunction()

# What whatever links a Rust static library links with it.
function(_rust_crate_link_runtime_libraries target)
    # Rust allocations go through AK so allocator overrides also apply across the FFI boundary.
    target_link_libraries(${target} INTERFACE AK)

    # Rust staticlibs bundle the standard library, which on Windows depends on system libraries.
    if (WIN32)
        target_link_libraries(${target} INTERFACE kernel32 ntdll Ws2_32 userenv)
    endif()
endfunction()

# _rust_crate_sync_ffi_headers(CRATE_NAME name CARGO_OUTPUT path FFI_OUTPUT_DIR dir FFI_HEADERS header...
#                              OUTPUT_VAR var [DEPENDS target...])
#
# Copies the FFI headers that the build script of CRATE_NAME writes into FFI_OUTPUT_DIR once CARGO_OUTPUT, the library
# of the cargo invocation that runs the build script, is built, and returns the copies in OUTPUT_VAR.
function(_rust_crate_sync_ffi_headers)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "CRATE_NAME;CARGO_OUTPUT;FFI_OUTPUT_DIR;OUTPUT_VAR" "FFI_HEADERS;DEPENDS")

    get_filename_component(cargo_output_dir "${ARG_CARGO_OUTPUT}" DIRECTORY)
    set(ffi_output "")
    set(sync_ffi_header_commands "")
    foreach(ffi_header IN LISTS ARG_FFI_HEADERS)
        list(APPEND ffi_output "${ARG_FFI_OUTPUT_DIR}/${ffi_header}")
        list(APPEND sync_ffi_header_commands
            COMMAND
                ${CMAKE_COMMAND}
                    -DCARGO_BUILD_SCRIPT_DIR=${cargo_output_dir}/build
                    -DCRATE_NAME=${ARG_CRATE_NAME}
                    -DFFI_HEADER=${ffi_header}
                    -DFFI_OUTPUT_DIR=${ARG_FFI_OUTPUT_DIR}
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sync_rust_ffi_header.cmake"
        )
    endforeach()

    # Keep Cargo's output separate from copy_if_different outputs. If they share an edge, Ninja's restat timestamp
    # may predate files generated by build.rs and listed in the Cargo depfile, causing the crate to be incorrectly
    # considered dirty on the next build.
    add_custom_command(
        OUTPUT ${ffi_output}
        ${sync_ffi_header_commands}
        DEPENDS "${ARG_CARGO_OUTPUT}" ${ARG_DEPENDS}
        COMMAND_EXPAND_LISTS
    )
    set(${ARG_OUTPUT_VAR} ${ffi_output} PARENT_SCOPE)
endfunction()

# sync_rust_crate_ffi_headers(CRATE_NAME name PRODUCED_BY crate FFI_OUTPUT_DIR dir FFI_HEADERS header...)
#
# Copies the FFI headers that the build script of CRATE_NAME writes into FFI_OUTPUT_DIR, for a crate that is built as a
# dependency of PRODUCED_BY, a crate import_rust_crate() imported, rather than imported itself. The copies are made once
# the import's archive is, by the target <CRATE_NAME>-ffi-headers, which whatever compiles against the headers depends
# on.
function(sync_rust_crate_ffi_headers)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "CRATE_NAME;PRODUCED_BY;FFI_OUTPUT_DIR" "FFI_HEADERS")

    get_target_property(producer_archive ${ARG_PRODUCED_BY} IMPORTED_LOCATION)
    _rust_crate_sync_ffi_headers(
        CRATE_NAME ${ARG_CRATE_NAME}
        CARGO_OUTPUT "${producer_archive}"
        DEPENDS ${ARG_PRODUCED_BY}-build
        FFI_OUTPUT_DIR "${ARG_FFI_OUTPUT_DIR}"
        FFI_HEADERS ${ARG_FFI_HEADERS}
        OUTPUT_VAR ffi_output
    )
    add_custom_target(${ARG_CRATE_NAME}-ffi-headers DEPENDS ${ffi_output})
endfunction()

# generate_rust_ffi_headers(MANIFEST_PATH path/to/Cargo.toml CRATE_NAME name FFI_OUTPUT_DIR dir FFI_HEADERS header...)
#
# Builds a crate whose build script writes FFI headers, and nothing else links, and copies the headers into
# FFI_OUTPUT_DIR by the target <name>-ffi-headers, which whatever compiles against them depends on. Such a crate lets
# the C++ that includes the headers of a large crate wait for the headers alone rather than for the crate. It is built
# in the directory of the crates import_rust_crate() builds, so it shares their builds of its dependencies.
function(generate_rust_ffi_headers)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "MANIFEST_PATH;CRATE_NAME;FFI_OUTPUT_DIR" "FFI_HEADERS")

    _rust_crate_common_setup(
        MANIFEST_PATH "${ARG_MANIFEST_PATH}"
        CRATE_NAME ${ARG_CRATE_NAME}
    )
    set(cargo_feature_flags "")
    set(output_lib "${cargo_output_dir}/lib${ARG_CRATE_NAME}.rlib")
    _rust_crate_build_command(OUTPUT "${output_lib}" DEPFILE "${cargo_output_dir}/lib${ARG_CRATE_NAME}.d")

    _rust_crate_sync_ffi_headers(
        CRATE_NAME ${ARG_CRATE_NAME}
        CARGO_OUTPUT "${output_lib}"
        FFI_OUTPUT_DIR "${ARG_FFI_OUTPUT_DIR}"
        FFI_HEADERS ${ARG_FFI_HEADERS}
        OUTPUT_VAR ffi_output
    )
    add_custom_target(${ARG_CRATE_NAME}-ffi-headers DEPENDS ${ffi_output})
endfunction()

# build_rust_binary(MANIFEST_PATH path/to/Cargo.toml CRATE_NAME name BINARY_NAME name OUTPUT_PATH_VAR var)
#
# Builds a Rust binary crate target using cargo and exposes the copied binary path through OUTPUT_PATH_VAR.
function(build_rust_binary)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "MANIFEST_PATH;CRATE_NAME;BINARY_NAME;OUTPUT_NAME;OUTPUT_PATH_VAR;FFI_OUTPUT_DIR" "FEATURES")

    if (NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "${ARG_BINARY_NAME}")
    endif()

    _rust_crate_common_setup(
        MANIFEST_PATH "${ARG_MANIFEST_PATH}"
        CRATE_NAME ${ARG_CRATE_NAME}
        FFI_OUTPUT_DIR "${ARG_FFI_OUTPUT_DIR}"
        TARGET_DIR "${CMAKE_BINARY_DIR}/cargo/binaries/${ARG_BINARY_NAME}"
    )

    set(cargo_feature_flags "")
    if (ARG_FEATURES)
        list(JOIN ARG_FEATURES "," cargo_features)
        list(APPEND cargo_feature_flags "--features=${cargo_features}")
    endif()

    set(cargo_binary "${cargo_output_dir}/${ARG_BINARY_NAME}${CMAKE_EXECUTABLE_SUFFIX}")
    set(depfile "${cargo_output_dir}/${ARG_BINARY_NAME}.d")
    set(output_binary "${CMAKE_BINARY_DIR}/bin/${ARG_OUTPUT_NAME}${CMAKE_EXECUTABLE_SUFFIX}")

    add_custom_command(
        OUTPUT "${cargo_binary}"
        COMMAND ${CMAKE_COMMAND}
            -DLOG=${cargo_output_dir}/cargo-${ARG_CRATE_NAME}.log
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_quiet.cmake"
            --
            ${CMAKE_COMMAND} -E env ${cargo_env} CARGO_TERM_PROGRESS_WHEN=never TERM=dumb
            "${RUST_CARGO}"
                rustc
                --bin ${ARG_BINARY_NAME}
                ${cargo_feature_flags}
                ${cargo_common_flags}
        DEPENDS "${manifest_path}"
            "${workspace_dir}/Cargo.lock" "${workspace_dir}/Cargo.toml"
            "${RUST_RUSTC}" "${CMAKE_SOURCE_DIR}/rust-toolchain.toml"
        DEPFILE "${depfile}"
        COMMENT "Building Rust binary ${ARG_BINARY_NAME}"
        JOB_POOL cargo
        COMMAND_EXPAND_LISTS
    )

    # See the corresponding comment in import_rust_crate(). In addition to avoiding misleading restat timestamps, this
    # lets copy_if_different suppress downstream rebuilds when Cargo recreates an identical binary.
    add_custom_command(
        OUTPUT "${output_binary}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cargo_binary}" "${output_binary}"
        DEPENDS "${cargo_binary}"
    )

    add_custom_target(${ARG_BINARY_NAME}-build DEPENDS "${output_binary}")

    if (ARG_OUTPUT_PATH_VAR)
        set(${ARG_OUTPUT_PATH_VAR} "${output_binary}" PARENT_SCOPE)
    endif()
endfunction()

function(test_rust_crate)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "MANIFEST_PATH;CRATE_NAME" "ENVIRONMENT")
    _rust_crate_common_setup(
        MANIFEST_PATH "${ARG_MANIFEST_PATH}"
        CRATE_NAME ${ARG_CRATE_NAME}
        TARGET_DIR "${CMAKE_BINARY_DIR}/cargo/tests/${ARG_CRATE_NAME}"
    )
    list(APPEND cargo_env ${ARG_ENVIRONMENT})
    # cargo test accepts harness arguments after --, not rustc arguments.
    list(FIND cargo_common_flags "--" rustc_flags_start)
    list(SUBLIST cargo_common_flags 0 ${rustc_flags_start} cargo_test_flags)
    # Unit tests exercise internal invariants even when using optimized build artifacts.
    list(APPEND cargo_env "CARGO_PROFILE_RELEASE_DEBUG_ASSERTIONS=true")
    add_custom_target(${ARG_CRATE_NAME}-test
        COMMAND ${CMAKE_COMMAND} -E env ${cargo_env}
            "${RUST_CARGO}" test --lib ${cargo_test_flags}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
    )
endfunction()

# Shared cargo setup for Rust build and test targets.
function(_rust_crate_common_setup)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "MANIFEST_PATH;CRATE_NAME;FFI_OUTPUT_DIR;TARGET_DIR" "")

    set(manifest_path "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_MANIFEST_PATH}")

    # Find the workspace Cargo.lock to track as a dependency.
    get_filename_component(workspace_dir "${manifest_path}" DIRECTORY)
    while(NOT EXISTS "${workspace_dir}/Cargo.lock")
        get_filename_component(workspace_dir "${workspace_dir}" DIRECTORY)
    endwhile()

    # Detect the Rust toolchain.
    find_program(RUST_CARGO cargo REQUIRED)
    find_program(RUST_RUSTC rustc REQUIRED)
    if (NOT DEFINED RUSTC_WRAPPER)
        set(RUSTC_WRAPPER "$ENV{RUSTC_WRAPPER}" CACHE FILEPATH "Path to a rustc wrapper program, e.g. sccache")
    endif()
    if (NOT RUSTC_WRAPPER)
        find_program(SCCACHE_PROGRAM sccache)
        if (SCCACHE_PROGRAM)
            set(RUSTC_WRAPPER "${SCCACHE_PROGRAM}" CACHE FILEPATH "Path to a rustc wrapper program, e.g. sccache" FORCE)
        endif()
    endif()
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

    if (ARG_TARGET_DIR)
        set(cargo_target_dir "${ARG_TARGET_DIR}")
    else()
        set(cargo_target_dir "${CMAKE_BINARY_DIR}/cargo/build")
    endif()
    set(cargo_output_dir "${cargo_target_dir}/${RUST_TARGET_TRIPLE}/${cargo_profile_dir}")

    # Build environment variables for cargo.
    set(cargo_env
        "CC_${target_underscore}=${CMAKE_C_COMPILER}"
        "CXX_${target_underscore}=${CMAKE_CXX_COMPILER}"
        "CARGO_BUILD_RUSTC=${RUST_RUSTC}"
    )

    list(APPEND cargo_env "CARGO_TARGET_${target_upper}_RUSTFLAGS=${RUSTC_TARGET_FLAGS}")

    # The optimized builds otherwise leave out every debug assertion, which then runs only in unit tests. A sanitizer
    # build is one that is run to find bugs, so it checks them too.
    if (ENABLE_ADDRESS_SANITIZER OR ENABLE_UNDEFINED_SANITIZER)
        list(APPEND cargo_env "CARGO_PROFILE_RELEASE_DEBUG_ASSERTIONS=true")
    endif()

    if (LADYBIRD_GENERATE_SYMBOL_FILES)
        list(APPEND cargo_env "CARGO_PROFILE_RELEASE_DEBUG=line-tables-only")
    endif()

    if (RUSTC_WRAPPER)
        list(APPEND cargo_env
            "RUSTC_WRAPPER=${RUSTC_WRAPPER}"
            "CARGO_INCREMENTAL=0"
        )
    endif()

    if (ARG_FFI_OUTPUT_DIR)
        list(APPEND cargo_env "FFI_OUTPUT_DIR=${ARG_FFI_OUTPUT_DIR}")
    endif()

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

    set(cargo_common_flags
        "--target=${RUST_TARGET_TRIPLE}"
        --package ${ARG_CRATE_NAME}
        --manifest-path "${manifest_path}"
        --target-dir "${cargo_target_dir}"
        ${cargo_profile_flag}
        --
        -Cdefault-linker-libraries=yes
        --emit=dep-info
    )

    # Populate the variable names used by the public helpers below.
    set(RUST_CARGO "${RUST_CARGO}" PARENT_SCOPE)

    set(cargo_common_flags "${cargo_common_flags}" PARENT_SCOPE)
    set(cargo_env "${cargo_env}" PARENT_SCOPE)
    set(cargo_output_dir "${cargo_output_dir}" PARENT_SCOPE)
    set(manifest_path "${manifest_path}" PARENT_SCOPE)
    set(workspace_dir "${workspace_dir}" PARENT_SCOPE)
endfunction()
