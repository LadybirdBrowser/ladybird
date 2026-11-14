# FIXME: Send wake-all-pending-transfers.patch upstream. RequestServer needs it to use CURLOPT_PIPEWAIT, and goes without
#        when it is missing.
# FIXME: Drop poll-idle-connection-input.patch once curl is updated past 8.23.0: It backports curl/curl@ef6b17cf12d4, which
#        keeps an HTTP/1.1 connection with stray input pending from being reused.
# NB: This is vcpkg's own curl port with two more patches. To update curl, update the version in vcpkg.json here.
set(ladybird_curl_patches "${CMAKE_CURRENT_LIST_DIR}/wake-all-pending-transfers.patch" "${CMAKE_CURRENT_LIST_DIR}/poll-idle-connection-input.patch")
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/curl")

macro(vcpkg_from_github)
    _vcpkg_from_github(${ARGN} ${ladybird_curl_patches})
endmacro()

include("${CURRENT_PORT_DIR}/portfile.cmake")
