# FIXME: Send the patch upstream. RequestServer needs it to use CURLOPT_PIPEWAIT, and goes without when it is missing.
# NB: This is vcpkg's own curl port with one more patch. To update curl, update the version in vcpkg.json here.
set(ladybird_curl_patch "${CMAKE_CURRENT_LIST_DIR}/wake-all-pending-transfers.patch")
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/curl")

macro(vcpkg_from_github)
    _vcpkg_from_github(${ARGN} "${ladybird_curl_patch}")
endmacro()

include("${CURRENT_PORT_DIR}/portfile.cmake")
