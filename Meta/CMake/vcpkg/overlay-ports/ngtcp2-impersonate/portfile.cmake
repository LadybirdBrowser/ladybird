# ngtcp2 for the curl port's browser impersonation, with the fork's patch that lets curl shape QUIC's transport
# parameters and initial packet number, and its crypto backend on the BoringSSL the curl port carries.
set(VCPKG_LIBRARY_LINKAGE static)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/ngtcp2/ngtcp2/releases/download/v${VERSION}/ngtcp2-${VERSION}.tar.bz2"
    FILENAME "ngtcp2-${VERSION}.tar.bz2"
    SHA512 fead05300f91fe79bbacb1f57ca7f5e390ec0af5f847bcf2800434e0fa9a895851b71cc9774b0ac04cdd4d660bb1c02ddfa88cb08287649f57ce327a434b4262
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${VERSION}"
    PATCHES
        ngtcp2.patch
)

# Only the include directory is handed over: ngtcp2 checks BoringSSL's presence against the headers alone, and
# linking its static libraries into the check would want the C++ runtime that a C check program lacks.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_LIB_ONLY=ON
        -DENABLE_SHARED_LIB=OFF
        -DENABLE_STATIC_LIB=ON
        -DENABLE_OPENSSL=OFF
        -DENABLE_BORINGSSL=ON
        -DENABLE_PICOTLS=OFF
        -DENABLE_WOLFSSL=OFF
        -DENABLE_GNUTLS=OFF
        -DBUILD_TESTING=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Libev=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Libnghttp3=ON
        -DCMAKE_INSTALL_DOCDIR=share/ngtcp2
        "-DBORINGSSL_INCLUDE_DIR=${CURRENT_INSTALLED_DIR}/include/boringssl-impersonate"
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_C_VISIBILITY_PRESET=hidden
        -DCMAKE_CXX_VISIBILITY_PRESET=hidden
        -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON
)
vcpkg_cmake_install()

foreach(prefix IN ITEMS "" "debug/")
    set(dir "${CURRENT_PACKAGES_DIR}/${prefix}")
    if(NOT IS_DIRECTORY "${dir}lib")
        continue()
    endif()
    file(MAKE_DIRECTORY "${dir}lib/${PORT}")
    foreach(library IN ITEMS libngtcp2.a libngtcp2_crypto_boringssl.a)
        file(RENAME "${dir}lib/${library}" "${dir}lib/${PORT}/${library}")
    endforeach()
    file(REMOVE_RECURSE "${dir}lib/cmake" "${dir}lib/pkgconfig")
endforeach()
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/include/${PORT}")
file(RENAME "${CURRENT_PACKAGES_DIR}/include/ngtcp2" "${CURRENT_PACKAGES_DIR}/include/${PORT}/ngtcp2")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/${PORT}/ngtcp2/ngtcp2.h" "#ifdef NGTCP2_STATICLIB" "#if 1")
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/ngtcp2"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
