# wuffs ships as a single-file library that is compiled into the including translation unit.
set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/wuffs-mirror-release-c
    REF "v${VERSION}"
    SHA512 d22136a1adf337573944eed917142c6bde877a09bd65738010c6c367c7a3fc9e4573fd4dc8469c93799fe1e3247760e65e64829e830871ee7b333fd72ebc629d
    HEAD_REF main
)

file(INSTALL "${SOURCE_PATH}/release/c/wuffs-v0.3.c" DESTINATION "${CURRENT_PACKAGES_DIR}/include/wuffs")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
