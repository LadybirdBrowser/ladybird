if (NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "" AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    set(_PDFJS_HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

find_path(PDFJS_DIR
    NAMES build/pdf.mjs
    HINTS ${_PDFJS_HINTS}
    PATH_SUFFIXES share/pdfjs
    DOC "Path to the Ladybird-patched PDF.js distribution"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PDFJS REQUIRED_VARS PDFJS_DIR)
mark_as_advanced(PDFJS_DIR)
