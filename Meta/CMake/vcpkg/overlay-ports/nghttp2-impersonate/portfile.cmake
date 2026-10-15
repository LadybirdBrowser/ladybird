# nghttp2 for the curl port's browser impersonation: curl-impersonate sends the HTTP/2 priority frames browsers
# send, through an API nghttp2 removed in 1.65. Built static, with every symbol hidden, for libcurl to carry.
set(VCPKG_LIBRARY_LINKAGE static)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/nghttp2/nghttp2/releases/download/v${VERSION}/nghttp2-${VERSION}.tar.bz2"
    FILENAME "nghttp2-${VERSION}.tar.bz2"
    SHA512 a328b4642c6ca4395adfcaaf4e6eb6dbd39fa7bd86f872a76260af59a5a830e0ff5ad015865d6bc00e0baa8e4d0d9a67b4b97e9d78e5e05d1c53522364e5e235
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${VERSION}"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_LIB_ONLY=ON
        -DENABLE_DOC=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_STATIC_LIBS=ON
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_C_VISIBILITY_PRESET=hidden
        -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Libngtcp2=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Libngtcp2_crypto_quictls=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Libnghttp3=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Systemd=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Jansson=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Libevent=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_LibXml2=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Jemalloc=ON
)
vcpkg_cmake_install()

# The headers and library move under the port's own name, clear of the nghttp2 port's include/nghttp2.
foreach(prefix IN ITEMS "" "debug/")
    set(dir "${CURRENT_PACKAGES_DIR}/${prefix}")
    if(NOT IS_DIRECTORY "${dir}lib")
        continue()
    endif()
    file(MAKE_DIRECTORY "${dir}lib/${PORT}")
    file(RENAME "${dir}lib/libnghttp2.a" "${dir}lib/${PORT}/libnghttp2.a")
    file(REMOVE_RECURSE "${dir}lib/cmake" "${dir}lib/pkgconfig")
endforeach()
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include/${PORT}")
file(RENAME "${CURRENT_PACKAGES_DIR}/include/nghttp2" "${CURRENT_PACKAGES_DIR}/include/${PORT}/nghttp2")
file(APPEND "${CURRENT_PACKAGES_DIR}/include/${PORT}/nghttp2/nghttp2ver.h" [[
#ifndef NGHTTP2_STATICLIB
#  define NGHTTP2_STATICLIB
#endif
]])
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share" "${CURRENT_PACKAGES_DIR}/share/doc")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
