# pdf.js is a prebuilt web asset — nothing to compile.
set(VCPKG_BUILD_TYPE release)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/mozilla/pdf.js/releases/download/v${VERSION}/pdfjs-${VERSION}-dist.zip"
    FILENAME "pdfjs-${VERSION}-dist.zip"
    SHA512 708da0d1e90058bfb974991c39fa46dbd81fa1882003d05964071be753866dc2502ff21c135170b42a915ad4704f9969863f2d0ce78f55658adc4a67b776779e
)

vcpkg_extract_source_archive_ex(
    OUT_SOURCE_PATH SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)

# Integrate the viewer with Ladybird's resource URLs and PDF transport.
z_vcpkg_apply_patches(
    SOURCE_PATH "${SOURCE_PATH}"
    PATCHES 0001-ladybird-embed.patch
)

# build/: pdf.mjs, pdf.worker.mjs, pdf.renderer.mjs, pdf.sandbox.mjs (no .map files)
file(GLOB BUILD_MJS "${SOURCE_PATH}/build/*.mjs")
file(INSTALL ${BUILD_MJS} DESTINATION "${CURRENT_PACKAGES_DIR}/share/pdfjs/build")

# web/: viewer HTML, scripts and CSS
file(INSTALL
    "${SOURCE_PATH}/web/viewer.html"
    "${SOURCE_PATH}/web/viewer.mjs"
    "${SOURCE_PATH}/web/viewer.css"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/pdfjs/web"
)

# web/images/: toolbar icons
file(INSTALL "${SOURCE_PATH}/web/images/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/pdfjs/web/images")

# web/wasm/: decoders, JavaScript fallbacks and their licenses
file(INSTALL "${SOURCE_PATH}/web/wasm/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/pdfjs/web/wasm")

# web/locale/: en-US only with a minimal locale.json
file(INSTALL "${SOURCE_PATH}/web/locale/en-US/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/pdfjs/web/locale/en-US")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/pdfjs/web/locale/locale.json"
    "{\"en-us\":\"en-US/viewer.ftl\"}")

set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

file(GLOB WASM_LICENSES "${SOURCE_PATH}/web/wasm/LICENSE_*")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" ${WASM_LICENSES})
