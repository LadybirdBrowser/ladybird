if(VCPKG_TARGET_IS_WINDOWS)
    # Upstream does not export symbols for Windows shared libraries.
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO vitaut/zmij
    REF "v${VERSION}"
    SHA512 f7c73c6cadb5588e717feac96ea6c4b0eb0bd1317b16008e3fe761af265808832ea03b2bc97b188200a5e14726d229fc589afaafee87f59fc290a6266a2b2a92
    HEAD_REF main
    PATCHES
        001-explicit-array-sizes.patch # https://github.com/vitaut/zmij/commit/5b87a80c60
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZMIJ_TEST=OFF
        -DZMIJ_EXAMPLE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/zmij")
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
