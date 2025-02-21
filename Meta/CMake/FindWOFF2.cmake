include(FindPackageHandleStandardArgs)

find_path(WOFF2_INCLUDE_DIR woff2/decode.h)

if (CMAKE_BUILD_TYPE MATCHES "Debug|RelWithDebInfo")
    set(LIBRARY_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib")
else()
    set(LIBRARY_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
endif()

set(WOFF2_LIBRARY "${LIBRARY_DIR}/woff2dec.lib")

find_package_handle_standard_args(WOFF2 DEFAULT_MSG WOFF2_LIBRARY WOFF2_INCLUDE_DIR)

if (WOFF2_FOUND AND NOT TARGET WOFF2::WOFF2)
    add_library(WOFF2::WOFF2 UNKNOWN IMPORTED)
    set_target_properties(WOFF2::WOFF2 PROPERTIES
        IMPORTED_LOCATION "${WOFF2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WOFF2_INCLUDE_DIR}")
    target_link_libraries(WOFF2::WOFF2 INTERFACE "${LIBRARY_DIR}/woff2common.lib" "${LIBRARY_DIR}/brotlidec.lib")
endif()
