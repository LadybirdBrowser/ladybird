# pdf.js is a prebuilt web asset — nothing to compile.
set(VCPKG_BUILD_TYPE release)

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/mozilla/pdf.js/releases/download/v${VERSION}/pdfjs-${VERSION}-dist.zip"
    FILENAME "pdfjs-${VERSION}-dist.zip"
    SHA512 66fecdb8a80d013b592c361e85abd7eeea5bc35f0131b771cc25a1878d7737704458dc715670aace3c7d3437f85388dff04796fe117eef01be81fd0d192f73a2
)

vcpkg_extract_source_archive_ex(
    OUT_SOURCE_PATH SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)

# Patch viewer.mjs so the internal Ladybird PDF viewer origin is accepted.
z_vcpkg_apply_patches(
    SOURCE_PATH "${SOURCE_PATH}"
    PATCHES 0001-ladybird-embed.patch
)

# build/: pdf.mjs, pdf.worker.mjs, pdf.sandbox.mjs (no .map files)
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

# web/locale/: en-US only with a minimal locale.json
file(INSTALL "${SOURCE_PATH}/web/locale/en-US/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/pdfjs/web/locale/en-US")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/pdfjs/web/locale/locale.json"
    "{\"en-us\":\"en-US/viewer.ftl\"}")

set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

file(WRITE "${CURRENT_PACKAGES_DIR}/share/pdfjs/copyright"
    "pdf.js is licensed under the Apache 2.0 License.\n"
    "See https://github.com/mozilla/pdf.js/blob/master/LICENSE\n")
