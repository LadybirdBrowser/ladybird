# Glib uses winapi functions not available in WindowsStore
string(REGEX MATCH "^([0-9]*[.][0-9]*)" GLIBMM_MAJOR_MINOR "${VERSION}")
vcpkg_download_distfile(GLIBMM_ARCHIVE
    URLS "https://ftp.gnome.org/pub/GNOME/sources/glibmm/${GLIBMM_MAJOR_MINOR}/glibmm-${VERSION}.tar.xz"
    FILENAME "glibmm-${VERSION}.tar.xz"
    SHA512 6f9ee91212077f3712a5ba99507479b5c99d021670e6bf298afc7239fafed8a40b3b17bfae96d9a7fa7fb199c3994b81b433c8275afe89839229a1fe20ba791e
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${GLIBMM_ARCHIVE}"
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dbuild-examples=false
        -Dmsvc14x-parallel-installable=false
)

vcpkg_install_meson()
vcpkg_copy_pdbs()

# intentionally 2.68 - glib does not install glibmm-2.7x files
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/lib/glibmm-2.68/proc"
    "${CURRENT_PACKAGES_DIR}/lib/glibmm-2.68/proc"
)

vcpkg_fixup_pkgconfig()

# Handle copyright and readmes
file(INSTALL "${SOURCE_PATH}/COPYING" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
file(INSTALL "${SOURCE_PATH}/README.md" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME readme.txt)
file(INSTALL "${SOURCE_PATH}/README.win32.md" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
