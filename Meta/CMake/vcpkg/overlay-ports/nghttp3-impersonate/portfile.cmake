# nghttp3 for the curl port's browser impersonation, with the fork's patch that lets curl order the SETTINGS a
# browser sends. Built static, with every symbol hidden, for libcurl to carry. The release tarball ships sfparse.
set(VCPKG_LIBRARY_LINKAGE static)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/ngtcp2/nghttp3/releases/download/v${VERSION}/nghttp3-${VERSION}.tar.bz2"
    FILENAME "nghttp3-${VERSION}.tar.bz2"
    SHA512 e2de0402f3b75f081f421052e8fc72d7a679fbab67b8b1ba383bbfc61c90e7ff58e475c563747712a680e367af237c47f3f890326a55440bfdd2e51d0ea1fb6c
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${VERSION}"
    PATCHES
        nghttp3.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_LIB_ONLY=ON
        -DENABLE_SHARED_LIB=OFF
        -DENABLE_STATIC_LIB=ON
        -DBUILD_TESTING=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_C_VISIBILITY_PRESET=hidden
        -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON
    MAYBE_UNUSED_VARIABLES
        BUILD_TESTING
)
vcpkg_cmake_install()

foreach(prefix IN ITEMS "" "debug/")
    set(dir "${CURRENT_PACKAGES_DIR}/${prefix}")
    if(NOT IS_DIRECTORY "${dir}lib")
        continue()
    endif()
    file(MAKE_DIRECTORY "${dir}lib/${PORT}")
    file(RENAME "${dir}lib/libnghttp3.a" "${dir}lib/${PORT}/libnghttp3.a")
    file(REMOVE_RECURSE "${dir}lib/cmake" "${dir}lib/pkgconfig")
endforeach()
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include/${PORT}")
file(RENAME "${CURRENT_PACKAGES_DIR}/include/nghttp3" "${CURRENT_PACKAGES_DIR}/include/${PORT}/nghttp3")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/${PORT}/nghttp3/nghttp3.h" "#ifdef NGHTTP3_STATICLIB" "#if 1")
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/man"
    "${CURRENT_PACKAGES_DIR}/share/doc"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
