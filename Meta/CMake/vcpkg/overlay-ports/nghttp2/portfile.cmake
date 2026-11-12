# FIXME: Drop this patch once nghttp2 offers an option for it: https://github.com/nghttp2/nghttp2/issues/2989
# NB: This is vcpkg's own nghttp2 port with one more patch. To update nghttp2, update the version in vcpkg.json here.
set(ladybird_nghttp2_patch "${CMAKE_CURRENT_LIST_DIR}/ignore-connection-specific-response-fields.patch")
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/nghttp2")

macro(vcpkg_from_github)
    _vcpkg_from_github(${ARGN} PATCHES "${ladybird_nghttp2_patch}")
endmacro()

include("${CURRENT_PORT_DIR}/portfile.cmake")
