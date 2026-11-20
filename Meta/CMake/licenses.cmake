include_guard()

set(licenses_text_file "${CMAKE_BINARY_DIR}/LICENSES.txt")

set(licenses_generator "${LADYBIRD_SOURCE_DIR}/Meta/generate-licenses.py")
set(licenses_arguments
    --output "${licenses_text_file}"
    --source-dir "${LADYBIRD_SOURCE_DIR}"
    --cargo "${RUST_CARGO}"
    --rust-target "${RUST_TARGET_TRIPLE}"
)
set(licenses_dependencies
    "${licenses_generator}"
    "${LADYBIRD_SOURCE_DIR}/LICENSE"
    "${LADYBIRD_SOURCE_DIR}/Cargo.lock"
    "${LADYBIRD_SOURCE_DIR}/Meta/CMake/vcpkg/dynamic-ports.txt"
)

if (NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "" AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    list(APPEND licenses_arguments
        --vcpkg-installed-dir "${VCPKG_INSTALLED_DIR}"
        --vcpkg-triplet "${VCPKG_TARGET_TRIPLET}"
    )
    list(APPEND licenses_dependencies "${VCPKG_INSTALLED_DIR}/vcpkg/status")
endif()

if (NOT ENABLE_NETWORK_DOWNLOADS)
    list(APPEND licenses_arguments --offline)
endif()

add_custom_command(
    OUTPUT "${licenses_text_file}"
    COMMAND ${Python3_EXECUTABLE} "${licenses_generator}" ${licenses_arguments}
    DEPENDS ${licenses_dependencies}
    COMMENT "Collecting the licenses of third-party software"
    JOB_POOL cargo
    VERBATIM
)

# about:licenses decompresses the file itself, and falls back to the uncompressed one.
set(brotli_hints "")
if (NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "" AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    list(APPEND brotli_hints "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/brotli")
endif()
find_program(BROTLI_EXECUTABLE brotli HINTS ${brotli_hints})

if (BROTLI_EXECUTABLE)
    set(LADYBIRD_LICENSES_FILE "${licenses_text_file}.br")
    add_custom_command(
        OUTPUT "${LADYBIRD_LICENSES_FILE}"
        COMMAND "${BROTLI_EXECUTABLE}" --best --force "--output=${LADYBIRD_LICENSES_FILE}" "${licenses_text_file}"
        DEPENDS "${licenses_text_file}"
        COMMENT "Compressing the licenses of third-party software"
        VERBATIM
    )
else()
    message(STATUS "Found no brotli tool, so the licenses of third-party software are installed uncompressed")
    set(LADYBIRD_LICENSES_FILE "${licenses_text_file}")
endif()

add_custom_target(generate_licenses ALL DEPENDS "${LADYBIRD_LICENSES_FILE}")
