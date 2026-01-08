string(REPLACE "." "_" curl_version "curl-${VERSION}")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO curl/curl
    REF ${curl_version}
    SHA512 ec2fa6c47d52feed943421b00e98370971bcc73b82842a85426ea9e42d36eaab51258a8d00197fdaaf5ec39e19385280fe387765f27e3b3dc1086c46236dc0bf
    HEAD_REF master
    PATCHES
        dependencies.patch
        ladybird-quirks-mode.patch
        pkgconfig-curl-config.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        http2       USE_NGHTTP2
        http3       USE_NGTCP2
        wolfssl     CURL_USE_WOLFSSL
        openssl     CURL_USE_OPENSSL
        openssl     CURL_CA_FALLBACK
        mbedtls     CURL_USE_MBEDTLS
        ssh         CURL_USE_LIBSSH2
        tool        BUILD_CURL_EXE
        c-ares      ENABLE_ARES
        sspi        CURL_WINDOWS_SSPI
        brotli      CURL_BROTLI
        idn2        USE_LIBIDN2
        winidn      USE_WIN32_IDN
        zstd        CURL_ZSTD
        psl         CURL_USE_LIBPSL
        gssapi      CURL_USE_GSSAPI
        gsasl       CURL_USE_GSASL
        gnutls      CURL_USE_GNUTLS
        rtmp        USE_LIBRTMP
        httpsrr     USE_HTTPSRR
        ssls-export USE_SSLS_EXPORT
    INVERTED_FEATURES
        ldap        CURL_DISABLE_LDAP
        ldap        CURL_DISABLE_LDAPS
        non-http    HTTP_ONLY
        websockets  CURL_DISABLE_WEBSOCKETS
)

if("ssl" IN_LIST FEATURES AND
    NOT "http3" IN_LIST FEATURES AND
    # (windows & !uwp) | mingw to match curl[ssl]'s "platform"
    ((VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_UWP) OR VCPKG_TARGET_IS_MINGW))
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

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS 
        "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        ${FEATURE_OPTIONS}
        ${OPTIONS}
        -DBUILD_TESTING=OFF
        -DENABLE_CURL_MANUAL=OFF
        -DIMPORT_LIB_SUFFIX=   # empty
        -DSHARE_LIB_OBJECT=OFF
        -DCURL_USE_PKGCONFIG=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Perl=ON
    MAYBE_UNUSED_VARIABLES
        PKG_CONFIG_EXECUTABLE
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()

if ("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES curl AUTO_CLEAN)
endif()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CURL)

vcpkg_fixup_pkgconfig()
set(namespec "curl")
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(namespec "libcurl")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libcurl.pc" " -lcurl" " -l${namespec}")
endif()
if(NOT DEFINED VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libcurl.pc" " -lcurl" " -l${namespec}-d")
endif()

#Fix install path
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
