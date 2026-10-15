# wuffs ships as a single-file library that is compiled into the including translation unit.
set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/wuffs-mirror-release-c
    REF "v${VERSION}"
    SHA512 5c646fdc8aa61c504ca5569e4c644924ae6b5340b95f4a30eb700fc08a1f006c3e81031fec405eaa08e5ac5c2699cbf88d7e54a193fee0759f044e42ba8334eb
    HEAD_REF main
)

file(INSTALL "${SOURCE_PATH}/release/c/wuffs-v0.3.c" DESTINATION "${CURRENT_PACKAGES_DIR}/include/wuffs")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
