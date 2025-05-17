vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libtom/libtommath
    REF "v${VERSION}"
    SHA512 3dbd7053a670afa563a069a9785f1aa4cab14a210bcd05d8fc7db25bd3dcce36b10a3f4f54ca92d75a694f891226f01bdf6ac15bacafeb93a8be6b04c579beb3
    HEAD_REF develop
    PATCHES
        bcrypt.patch
        import-lib.patch
        has-set-double.patch # Remove in next release.
        msvc-dce.patch # This is a won't fix, see https://github.com/libtom/libtommath/blob/develop/s_mp_rand_platform.c#L120-L138
        stdc-iec-559.patch # Define __STDC_IEC_559__ like glibc and musl do.
        install-pc.patch # Always install pkgconfig file, even when building statically.
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/${PORT}")
vcpkg_fixup_pkgconfig()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
