vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ximion/appstream
    REF "v${VERSION}"
    SHA512 80f3b7b9279152ce271bab61e97a41268d5dc5d977dc9488fc187df90077ac1a81169201d3d1a7a5578d36e962321035bfe34106486c2ac3d684621b40338de6
    HEAD_REF main
    PATCHES
      remove-uneeded-directories.patch
)

set(GLIB_TOOLS_DIR "${CURRENT_HOST_INSTALLED_DIR}/tools/glib")

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
      -Dsystemd=false
      -Dapidocs=false
      -Dinstall-docs=false
      -Dstemming=false
      -Dsvg-support=false
      -Dgir=false
    ADDITIONAL_BINARIES
       gperf='${CURRENT_HOST_INSTALLED_DIR}/tools/gperf/gperf${HOST_EXECUTABLE_SUFFIX}'
       glib-mkenums='${GLIB_TOOLS_DIR}/glib-mkenums'
       glib-compile-resources='${GLIB_TOOLS_DIR}/glib-compile-resources${VCPKG_HOST_EXECUTABLE_SUFFIX}'
)

vcpkg_install_meson()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
