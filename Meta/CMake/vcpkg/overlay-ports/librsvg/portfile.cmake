# port update requires rust/cargo

string(REGEX REPLACE "^([0-9]*[.][0-9]*)[.].*" "\\1" MAJOR_MINOR "${VERSION}")

# NOTE: Using GitHub mirror to avoid Anubis check failure on GNOME GitLab
# https://github.com/microsoft/vcpkg/issues/48350
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO GNOME/librsvg
    REF librsvg-gtk-${VERSION}
    SHA512 1fe06d7e745a53f3aee7b1942f7551c5716ec6abf328fa395006a7aede9f4ef242d604d5f8069c397d86ec3ac095daf49b18b2b34abc67fdcd4a113207fd6a96
    HEAD_REF master # branch name
    PATCHES
        fix-libxml2-2.13.5.patch
        meson-pkgconfig-and-def-file.patch
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-Dtests=disabled"
        "-Drsvg-convert=disabled"
        "-Drsvg-view-3=disabled"
    ADDITIONAL_BINARIES
        glib-mkenums='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-mkenums'
)

vcpkg_install_meson()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(COPY "${CURRENT_PORT_DIR}/unofficial-librsvg-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/unofficial-librsvg")
file(COPY "${CURRENT_PORT_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
