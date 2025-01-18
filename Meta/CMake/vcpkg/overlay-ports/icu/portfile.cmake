if(NOT VCPKG_TARGET_IS_WINDOWS)
    message(WARNING "${PORT} currently requires the following programs from the system package manager:
    autoconf automake autoconf-archive
On Debian and Ubuntu derivatives:
    sudo apt-get install autoconf automake autoconf-archive
On recent Red Hat and Fedora derivatives:
    sudo dnf install autoconf automake autoconf-archive
On Arch Linux and derivatives:
    sudo pacman -S autoconf automake autoconf-archive
On Alpine:
    apk add autoconf automake autoconf-archive
On macOS:
    brew install autoconf automake autoconf-archive\n")
endif()

string(REGEX MATCH "^[0-9]*" ICU_VERSION_MAJOR "${VERSION}")
string(REPLACE "." "_" VERSION2 "${VERSION}")
string(REPLACE "." "-" VERSION3 "${VERSION}")

vcpkg_download_distfile(
    ARCHIVE
    URLS "https://github.com/unicode-org/icu/releases/download/release-${VERSION3}/icu4c-${VERSION2}-src.tgz"
    FILENAME "icu4c-${VERSION2}-src.tgz"
    SHA512 e6c7876c0f3d756f3a6969cad9a8909e535eeaac352f3a721338b9cbd56864bf7414469d29ec843462997815d2ca9d0dab06d38c37cdd4d8feb28ad04d8781b0
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        disable-escapestr-tool.patch
        remove-MD-from-configure.patch
        fix_parallel_build_on_windows.patch
        fix-extra.patch
        mingw-dll-install.patch
        disable-static-prefix.patch # https://gitlab.kitware.com/cmake/cmake/-/issues/16617; also mingw.
        fix-win-build.patch
        vcpkg-cross-data.patch
        darwin-rpath.patch
        mingw-strict-ansi.diff # backport of https://github.com/unicode-org/icu/pull/3003
)

vcpkg_find_acquire_program(PYTHON3)
set(ENV{PYTHON} "${PYTHON3}")

vcpkg_list(SET CONFIGURE_OPTIONS)
vcpkg_list(SET BUILD_OPTIONS)

if(VCPKG_TARGET_IS_EMSCRIPTEN)
    vcpkg_list(APPEND CONFIGURE_OPTIONS --disable-extras)
    vcpkg_list(APPEND BUILD_OPTIONS "PKGDATA_OPTS=--without-assembly -O ../data/icupkg.inc")
elseif(VCPKG_TARGET_IS_UWP)
    vcpkg_list(APPEND CONFIGURE_OPTIONS --disable-extras ac_cv_func_tzset=no ac_cv_func__tzset=no)
    string(APPEND VCPKG_C_FLAGS " -DU_PLATFORM_HAS_WINUWP_API=1")
    string(APPEND VCPKG_CXX_FLAGS " -DU_PLATFORM_HAS_WINUWP_API=1")
    vcpkg_list(APPEND BUILD_OPTIONS "PKGDATA_OPTS=--windows-uwp-build -O ../data/icupkg.inc")
elseif(VCPKG_TARGET_IS_OSX AND VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_list(APPEND CONFIGURE_OPTIONS --enable-rpath)
    if(DEFINED CMAKE_INSTALL_NAME_DIR)
        vcpkg_list(APPEND BUILD_OPTIONS "ID_PREFIX=${CMAKE_INSTALL_NAME_DIR}")
    endif()
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND CONFIGURE_OPTIONS --enable-icu-build-win)
endif()

if("tools" IN_LIST FEATURES)
  list(APPEND CONFIGURE_OPTIONS --enable-tools)
else()
  list(APPEND CONFIGURE_OPTIONS --disable-tools)
endif()
if(CMAKE_HOST_WIN32 AND VCPKG_TARGET_IS_MINGW AND NOT HOST_TRIPLET MATCHES "mingw")
    # Assuming no cross compiling because the host (windows) pkgdata tool doesn't
    # use the '/' path separator when creating compiler commands for mingw bash.
elseif(VCPKG_CROSSCOMPILING)
    set(TOOL_PATH "${CURRENT_HOST_INSTALLED_DIR}/tools/${PORT}")
    # convert to unix path
    string(REGEX REPLACE "^([a-zA-Z]):/" "/\\1/" _VCPKG_TOOL_PATH "${TOOL_PATH}")
    list(APPEND CONFIGURE_OPTIONS "--with-cross-build=${_VCPKG_TOOL_PATH}")
endif()

vcpkg_configure_make(
    SOURCE_PATH "${SOURCE_PATH}"
    PROJECT_SUBPATH source
    AUTOCONFIG
    DETERMINE_BUILD_TRIPLET
    ADDITIONAL_MSYS_PACKAGES autoconf-archive
    OPTIONS
        ${CONFIGURE_OPTIONS}
        --disable-samples
        --disable-tests
        --disable-layoutex
    OPTIONS_RELEASE
        --disable-debug
        --enable-release
    OPTIONS_DEBUG
        --enable-debug
        --disable-release
)
vcpkg_install_make(OPTIONS ${BUILD_OPTIONS})

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/share"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/lib/icu"
    "${CURRENT_PACKAGES_DIR}/debug/lib/icu"
    "${CURRENT_PACKAGES_DIR}/debug/lib/icud")

file(GLOB TEST_LIBS
    "${CURRENT_PACKAGES_DIR}/lib/*test*"
    "${CURRENT_PACKAGES_DIR}/debug/lib/*test*")
if(TEST_LIBS)
    file(REMOVE ${TEST_LIBS})
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    # force U_STATIC_IMPLEMENTATION macro
    foreach(HEADER utypes.h utf_old.h platform.h)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/unicode/${HEADER}" "defined(U_STATIC_IMPLEMENTATION)" "1")
    endforeach()
endif()

# Install executables from /tools/icu/sbin to /tools/icu/bin on unix (/bin because icu require this for cross compiling)
if(VCPKG_TARGET_IS_LINUX OR VCPKG_TARGET_IS_OSX AND "tools" IN_LIST FEATURES)
    vcpkg_copy_tools(
        TOOL_NAMES icupkg gennorm2 gencmn genccode gensprep
        SEARCH_DIR "${CURRENT_PACKAGES_DIR}/tools/icu/sbin"
        DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin"
    )
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/tools/icu/sbin"
    "${CURRENT_PACKAGES_DIR}/tools/icu/debug")

# To cross compile, we need some files at specific positions. So lets copy them
file(GLOB CROSS_COMPILE_DEFS "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/config/icucross.*")
file(INSTALL ${CROSS_COMPILE_DEFS} DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/config")

file(GLOB RELEASE_DLLS "${CURRENT_PACKAGES_DIR}/lib/*icu*${ICU_VERSION_MAJOR}.dll")
file(COPY ${RELEASE_DLLS} DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin")

# copy dlls
file(GLOB RELEASE_DLLS "${CURRENT_PACKAGES_DIR}/lib/*icu*${ICU_VERSION_MAJOR}.dll")
file(COPY ${RELEASE_DLLS} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
if(NOT VCPKG_BUILD_TYPE)
    file(GLOB DEBUG_DLLS "${CURRENT_PACKAGES_DIR}/debug/lib/*icu*${ICU_VERSION_MAJOR}.dll")
    file(COPY ${DEBUG_DLLS} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

# remove any remaining dlls in /lib
file(GLOB DUMMY_DLLS "${CURRENT_PACKAGES_DIR}/lib/*.dll" "${CURRENT_PACKAGES_DIR}/debug/lib/*.dll")
if(DUMMY_DLLS)
    file(REMOVE ${DUMMY_DLLS})
endif()

vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/icu/bin/icu-config" "${CURRENT_INSTALLED_DIR}" "`dirname $0`/../../../" IGNORE_UNCHANGED)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
