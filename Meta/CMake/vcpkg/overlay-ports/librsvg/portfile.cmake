# port update requires rust/cargo

string(REGEX REPLACE "^([0-9]*[.][0-9]*)[.].*" "\\1" MAJOR_MINOR "${VERSION}")
vcpkg_download_distfile(ARCHIVE
    URLS "https://download.gnome.org/sources/librsvg/${MAJOR_MINOR}/librsvg-${VERSION}.tar.xz"
         "https://www.mirrorservice.org/sites/ftp.gnome.org/pub/GNOME/sources/librsvg/${MAJOR_MINOR}/librsvg-${VERSION}.tar.xz"
    FILENAME "librsvg-${VERSION}.tar.xz"
    SHA512 db0563d8e0edaae642a6b2bcd239cf54191495058ac8c7ff614ebaf88c0e30bd58dbcd41f58d82a9d5ed200ced45fc5bae22f2ed3cf3826e9348a497009e1280
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        fix-libxml2-2.13.5.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/config.h.linux" DESTINATION "${SOURCE_PATH}")

vcpkg_find_acquire_program(PKGCONFIG)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    file(GLOB_RECURSE pc_files "${CURRENT_PACKAGES_DIR}/*.pc")
    foreach(pc_file IN LISTS pc_files)
        vcpkg_replace_string("${pc_file}" " -lm" "")
    endforeach()
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(COPY "${CURRENT_PORT_DIR}/unofficial-librsvg-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/unofficial-librsvg")
file(COPY "${CURRENT_PORT_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
