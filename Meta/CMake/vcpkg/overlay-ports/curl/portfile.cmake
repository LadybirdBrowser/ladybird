string(REPLACE "." "_" curl_version "curl-${VERSION}")

# Everywhere but Windows, libcurl carries curl-impersonate's browser TLS and HTTP/2 fingerprints: its patch on
# curl, and private static builds of its patched BoringSSL, nghttp2, nghttp3 and ngtcp2, which the rest of the
# tree never sees. Windows keeps the registry's curl on the openssl port.
if(VCPKG_TARGET_IS_WINDOWS)
    set(IMPERSONATE OFF)
else()
    set(IMPERSONATE ON)
endif()

# dependencies.patch pins curl's finders to pkg-config and CMake config files, which the private builds don't
# ship; restore-finder-paths.patch gives the nghttp2, nghttp3 and ngtcp2 finders their explicit paths back. And
# boringssl-unexpected-eof.patch has the BoringSSL backend take a transport EOF without close_notify for the end of
# the data, as RequestServer's SSL_OP_IGNORE_UNEXPECTED_EOF makes OpenSSL do: servers like WPT's close that way.
set(IMPERSONATE_PATCHES "")
if(IMPERSONATE)
    set(IMPERSONATE_PATCHES impersonate.patch restore-finder-paths.patch boringssl-unexpected-eof.patch)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO curl/curl
    REF ${curl_version}
    SHA512 6284a82c908329fda4b5a8c2efe409bf477a1fd79a0b71c04ae62ee407a4f00751722e98c1b73c728b0b79ee032eed1d1903876783c48cc6be71166619c025a7
    HEAD_REF master
    PATCHES
        dependencies.patch
        ${IMPERSONATE_PATCHES}
)
# The on-the-fly tarballs do not carry the details of release tarballs.
vcpkg_replace_string("${SOURCE_PATH}/include/curl/curlver.h" [[-DEV"]] [["]])
vcpkg_replace_string("${SOURCE_PATH}/include/curl/curlver.h" [[LIBCURL_TIMESTAMP "[unreleased]"]] [[LIBCURL_TIMESTAMP "[vcpkg]"]])

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        brotli      CURL_BROTLI
        c-ares      ENABLE_ARES
        gnutls      CURL_USE_GNUTLS
        gsasl       CURL_USE_GSASL
        gssapi      CURL_USE_GSSAPI
        gssapi      VCPKG_LOCK_FIND_PACKAGE_GSS
        http2       USE_NGHTTP2
        http2       VCPKG_LOCK_FIND_PACKAGE_NGHTTP2
        http3       USE_NGTCP2
        httpsrr     USE_HTTPSRR
        idn2        USE_LIBIDN2
        idn2        VCPKG_LOCK_FIND_PACKAGE_Libidn2
        ldap        VCPKG_LOCK_FIND_PACKAGE_LDAP
        mbedtls     CURL_USE_MBEDTLS
        openssl     CURL_CA_FALLBACK
        openssl     CURL_USE_OPENSSL
        psl         CURL_USE_LIBPSL
        ssh         CURL_USE_LIBSSH2
        ssh         VCPKG_LOCK_FIND_PACKAGE_Libssh2
        ssls-export USE_SSLS_EXPORT
        sspi        CURL_WINDOWS_SSPI
        tool        BUILD_CURL_EXE
        winidn      USE_WIN32_IDN
        wolfssl     CURL_USE_WOLFSSL
        zstd        CURL_ZSTD
    INVERTED_FEATURES
        ldap        CURL_DISABLE_LDAP
        ldap        CURL_DISABLE_LDAPS
        non-http    HTTP_ONLY
        websockets  CURL_DISABLE_WEBSOCKETS
)

if("ssl" IN_LIST FEATURES AND
    NOT "http3" IN_LIST FEATURES AND
    # Match curl[ssl]'s "platform": "windows & !uwp"
    (VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_UWP))
    list(APPEND FEATURE_OPTIONS -DCURL_USE_SCHANNEL=ON)
endif()

if("http3" IN_LIST FEATURES AND
    ("wolfssl" IN_LIST FEATURES OR
     "mbedtls" IN_LIST FEATURES OR
     "gnutls" IN_LIST FEATURES))
    message(FATAL_ERROR "http3 is incompatible with curl multi-ssl, preventing combination with wolfssl, mbedtls or \
gnutls in vcpkg's curated registry. To use curl http3 on ngtcp2 on one of the other TLS backends, author an \
overlay-port which exchanges curl[ssl]'s and curl[http3]'s openssl dependencies with the backend you want.")
endif()

set(OPTIONS "")

if(VCPKG_TARGET_IS_UWP)
    list(APPEND OPTIONS
        -DCURL_DISABLE_TELNET=ON
        -DENABLE_UNIX_SOCKETS=OFF
    )
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND OPTIONS -DENABLE_UNICODE=ON)
endif()

if(VCPKG_TARGET_IS_APPLE AND
    ("openssl" IN_LIST FEATURES OR "gnutls" IN_LIST FEATURES))
    list(APPEND OPTIONS -DUSE_APPLE_SECTRUST=ON)
endif()

set(OPTIONS_RELEASE "")
set(OPTIONS_DEBUG "")
if(IMPERSONATE)
    set(deps "${CURRENT_INSTALLED_DIR}")
    # curl's configure checks link test programs against the TLS library. BoringSSL is C++, so a C test program
    # can't link it; the answers are given up front, as curl-impersonate's own build gives them.
    list(APPEND OPTIONS
        -DCURL_IMPERSONATE_VERSION=2.2.3
        "-DOPENSSL_INCLUDE_DIR=${deps}/include/boringssl-impersonate"
        -DHAVE_BORINGSSL=1
        -DHAVE_AWSLC=0
        -DHAVE_LIBRESSL=0
        -DHAVE_DES_ECB_ENCRYPT=1
        -DHAVE_SSL_SET0_WBIO=1
        -DHAVE_SSL_SET1_ECH_CONFIG_LIST=1
        -DHAVE_SSL_SET_QUIC_USE_LEGACY_CODEPOINT=1
        -DHAVE_OPENSSL_SRP=0
        -DUSE_ECH=ON
        "-DNGHTTP2_INCLUDE_DIR=${deps}/include/nghttp2-impersonate"
        -DNGHTTP2_USE_STATIC_LIBS=ON
        "-DNGHTTP3_INCLUDE_DIR=${deps}/include/nghttp3-impersonate"
        -DNGHTTP3_USE_STATIC_LIBS=ON
        "-DNGTCP2_INCLUDE_DIR=${deps}/include/ngtcp2-impersonate"
        -DNGTCP2_USE_STATIC_LIBS=ON
    )
    foreach(config IN ITEMS RELEASE DEBUG)
        if(config STREQUAL "DEBUG")
            set(lib "${deps}/debug/lib")
        else()
            set(lib "${deps}/lib")
        endif()
        list(APPEND OPTIONS_${config}
            "-DOPENSSL_SSL_LIBRARY=${lib}/boringssl-impersonate/libssl.a"
            "-DOPENSSL_CRYPTO_LIBRARY=${lib}/boringssl-impersonate/libcrypto.a"
            "-DNGHTTP2_LIBRARY=${lib}/nghttp2-impersonate/libnghttp2.a"
            "-DNGHTTP3_LIBRARY=${lib}/nghttp3-impersonate/libnghttp3.a"
            "-DNGTCP2_LIBRARY=${lib}/ngtcp2-impersonate/libngtcp2.a"
            "-DNGTCP2_CRYPTO_BORINGSSL_LIBRARY=${lib}/ngtcp2-impersonate/libngtcp2_crypto_boringssl.a"
        )
    endforeach()
    # BoringSSL's headers go ahead of every other include directory, so <openssl/ssl.h> never resolves to the
    # openssl port's headers next door.
    set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} -I${deps}/include/boringssl-impersonate")
    set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} -I${deps}/include/boringssl-impersonate")
endif()

vcpkg_find_acquire_program(PKGCONFIG)
set(ENV{PKG_CONFIG} "${PKGCONFIG}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        ${FEATURE_OPTIONS}
        ${OPTIONS}
        -DBUILD_TESTING=OFF
        -DENABLE_CURL_MANUAL=OFF
        -DIMPORT_LIB_SUFFIX=   # empty
        -DSHARE_LIB_OBJECT=OFF
        -DCURL_USE_CMAKECONFIG=ON
        -DCURL_USE_PKGCONFIG=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Perl=ON
    OPTIONS_RELEASE
        ${OPTIONS_RELEASE}
    OPTIONS_DEBUG
        ${OPTIONS_DEBUG}
    MAYBE_UNUSED_VARIABLES
        HAVE_AWSLC
        HAVE_LIBRESSL
        HAVE_OPENSSL_SRP
        VCPKG_LOCK_FIND_PACKAGE_GSS
        VCPKG_LOCK_FIND_PACKAGE_LDAP
        VCPKG_LOCK_FIND_PACKAGE_Libidn2
        VCPKG_LOCK_FIND_PACKAGE_Libssh2
        VCPKG_LOCK_FIND_PACKAGE_NGHTTP2
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CURL)
if(IMPERSONATE)
    # libcurl carries nghttp3 and ngtcp2, so a consumer of the shared library has nothing to find; their finders
    # would only turn up whatever the host happens to have.
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${PORT}/CURLConfig.cmake" "  find_dependency(NGHTTP3 MODULE)\n" "")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${PORT}/CURLConfig.cmake" "  find_dependency(NGTCP2 MODULE COMPONENTS \"BoringSSL\")\n" "")
endif()

if(IMPERSONATE)
    # The private builds ship no pkg-config files, and libcurl carries them, so its own file stops requiring them.
    foreach(pc IN ITEMS "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libcurl.pc" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libcurl.pc")
        if(EXISTS "${pc}")
            foreach(module IN ITEMS libngtcp2_crypto_boringssl libngtcp2 libnghttp3 libnghttp2)
                vcpkg_replace_string("${pc}" ",${module}" "" IGNORE_UNCHANGED)
                vcpkg_replace_string("${pc}" "${module}," "" IGNORE_UNCHANGED)
            endforeach()
        endif()
    endforeach()
endif()

vcpkg_fixup_pkgconfig()
set(namespec "curl")
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(namespec "libcurl")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libcurl.pc" " -lcurl" " -l${namespec}")
endif()
if(NOT DEFINED VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libcurl.pc" " -lcurl" " -l${namespec}-d")
endif()

if ("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES curl AUTO_CLEAN)
endif()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/curl-config" "${CURRENT_PACKAGES_DIR}" "\${prefix}")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/curl-config" "${CURRENT_INSTALLED_DIR}" "\${prefix}" IGNORE_UNCHANGED)
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/bin/curl-config" "\nprefix='\${prefix}'" [=[prefix=$(CDPATH= cd -- "$(dirname -- "$0")"/../../.. && pwd -P)]=])
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin")
file(RENAME "${CURRENT_PACKAGES_DIR}/bin/curl-config" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin/curl-config")
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/bin/curl-config")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "${CURRENT_PACKAGES_DIR}" "\${prefix}")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "${CURRENT_INSTALLED_DIR}" "\${prefix}" IGNORE_UNCHANGED)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\nprefix='\${prefix}/debug'" [=[prefix=$(CDPATH= cd -- "$(dirname -- "$0")"/../../../.. && pwd -P)]=])
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\nexec_prefix=\"\${prefix}\"" "\nexec_prefix=\"\${prefix}/debug\"")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "-lcurl" "-l${namespec}-d")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "curl." "curl-d.")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug/bin")
    file(RENAME "${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug/bin/curl-config")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static" OR NOT VCPKG_TARGET_IS_WINDOWS)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/curl/curl.h"
        "#ifdef CURL_STATICLIB"
        "#if 1"
    )
endif()

file(INSTALL "${CURRENT_PORT_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CURRENT_PORT_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(READ "${SOURCE_PATH}/lib/curlx/inet_ntop.c" inet_ntop_c)
string(REGEX REPLACE "#i.*" "" inet_ntop_c "${inet_ntop_c}")
set(inet_ntop_copyright "${CURRENT_BUILDTREES_DIR}/inet_ntop.c and inet_pton.c Notice")
file(WRITE "${inet_ntop_copyright}" "${inet_ntop_c}")

vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/COPYING"
        "${inet_ntop_copyright}"
)
