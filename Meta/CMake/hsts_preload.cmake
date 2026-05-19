include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)

set(HSTS_PRELOAD_PATH "${LADYBIRD_CACHE_DIR}/HSTSPreload" CACHE PATH "Download location for HSTS preload files")
set(HSTS_PRELOAD_DATA_URL "https://raw.githubusercontent.com/chromium/chromium/main/net/http/transport_security_state_static.json")
set(HSTS_PRELOAD_DATA_PATH "${HSTS_PRELOAD_PATH}/transport_security_state_static.json")
set(HSTS_PRELOAD_DATA_HEADER HSTSPreloadData.h)
set(HSTS_PRELOAD_DATA_IMPLEMENTATION HSTSPreloadData.cpp)
if (ENABLE_NETWORK_DOWNLOADS)
    download_file("${HSTS_PRELOAD_DATA_URL}" "${HSTS_PRELOAD_DATA_PATH}")
else()
    message(STATUS "Skipping download of ${HSTS_PRELOAD_DATA_URL}, expecting it to be in ${HSTS_PRELOAD_DATA_PATH}")
endif()
invoke_py_generator(
    "HSTSPreloadData"
    "generate_hsts_preload_data.py"
    "${HSTS_PRELOAD_PATH}/"
    "${HSTS_PRELOAD_DATA_HEADER}"
    "${HSTS_PRELOAD_DATA_IMPLEMENTATION}"
    arguments -p "${HSTS_PRELOAD_DATA_PATH}"
)
set(HSTS_PRELOAD_SOURCES
    ${HSTS_PRELOAD_DATA_HEADER}
    ${HSTS_PRELOAD_DATA_IMPLEMENTATION}
)
