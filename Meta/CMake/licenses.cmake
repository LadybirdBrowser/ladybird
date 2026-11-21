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

# Build Release publishes the source archives of the LGPL ports of a distribution build, so LICENSES.txt points to them.
if (LADYBIRD_VCPKG_TYPE STREQUAL "distribution" AND NOT LADYBIRD_VERSION STREQUAL "")
    list(APPEND licenses_arguments --release)
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

# The source archives of the LGPL ports, which distribution builds ship as shared libraries, at the addresses that
# LICENSES.txt of a release names, for the Build Release workflow to hand to release-tools.
if (LADYBIRD_VCPKG_TYPE STREQUAL "distribution" AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
    add_custom_target(third_party_sources
        COMMAND ${Python3_EXECUTABLE} "${licenses_generator}"
            --source-dir "${LADYBIRD_SOURCE_DIR}"
            --vcpkg-installed-dir "${VCPKG_INSTALLED_DIR}"
            --vcpkg-triplet "${VCPKG_TARGET_TRIPLET}"
            --vcpkg-root "$ENV{VCPKG_ROOT}"
            --third-party-sources "${CMAKE_BINARY_DIR}/third-party-sources.tar"
        COMMENT "Packaging the source code of the LGPL ports"
        VERBATIM
    )
endif()
