vcpkg_check_linkage(
  ONLY_STATIC_LIBRARY
)

vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO google/woff2
  REF v1.0.2
  SHA512 c788bba1530aec463e755e901f9342f4b599e3a07f54645fef1dc388ab5d5c30625535e5dd38e9e792e04a640574baa50eeefb6b7338ab403755f4a4e0c3044d
  HEAD_REF master
  PATCHES
    0001-unofficial-brotli.patch
)

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DCANONICAL_PREFIXES=ON
    -DCMAKE_POLICY_DEFAULT_CMP0057=NEW
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

file(COPY "${CURRENT_PACKAGES_DIR}/bin/" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/woff2")
file(REMOVE_RECURSE
  "${CURRENT_PACKAGES_DIR}/bin"
  "${CURRENT_PACKAGES_DIR}/debug/bin"
  "${CURRENT_PACKAGES_DIR}/debug/include"
)

vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/woff2")

vcpkg_fixup_pkgconfig()
# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/woff2" RENAME copyright)
