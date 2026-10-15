# BoringSSL for the curl port's browser impersonation. It is built static, with every symbol hidden, so that libcurl
# can carry it without exporting anything that clashes with the openssl port, which the rest of the tree links.
set(VCPKG_LIBRARY_LINKAGE static)

set(BORINGSSL_COMMIT 156c7b75ae9b8c3b3f847acf264f17594c3859fb)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/google/boringssl/archive/${BORINGSSL_COMMIT}.zip"
    FILENAME "boringssl-${BORINGSSL_COMMIT}.zip"
    SHA512 1bd18f7344d6b30ff58f7e4319d7932ba915f235ee7552a997fc59977a863fc66ab3bbcc5020584cf0ef0b17af0bc2bcc87e083c7cd71993c9fc78e5af469767
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${BORINGSSL_COMMIT}"
    PATCHES
        boringssl.patch
        0002-remove-Werror.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTING=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_C_VISIBILITY_PRESET=hidden
        -DCMAKE_CXX_VISIBILITY_PRESET=hidden
        -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON
)
vcpkg_cmake_install()

# The headers and libraries move under the port's own name: the openssl port owns include/openssl and lib/libssl.
foreach(prefix IN ITEMS "" "debug/")
    set(dir "${CURRENT_PACKAGES_DIR}/${prefix}")
    if(NOT IS_DIRECTORY "${dir}lib")
        continue()
    endif()
    file(MAKE_DIRECTORY "${dir}lib/${PORT}")
    foreach(library IN ITEMS libssl.a libcrypto.a)
        file(RENAME "${dir}lib/${library}" "${dir}lib/${PORT}/${library}")
    endforeach()
    file(REMOVE_RECURSE "${dir}lib/cmake" "${dir}lib/pkgconfig")
endforeach()
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include/${PORT}")
file(RENAME "${CURRENT_PACKAGES_DIR}/include/openssl" "${CURRENT_PACKAGES_DIR}/include/${PORT}/openssl")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
