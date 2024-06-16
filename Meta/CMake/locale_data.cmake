include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)

set(CLDR_VERSION "45.0.0")
set(CLDR_SHA256 "ba934cdd40ad4fb6439004c7e746bef97fe2b597db1040fcaa6c7d0647742c1b")

set(CLDR_PATH "${SERENITY_CACHE_DIR}/CLDR" CACHE PATH "Download location for CLDR files")
set(CLDR_VERSION_FILE "${CLDR_PATH}/version.txt")

set(CLDR_ZIP_URL "https://github.com/unicode-org/cldr-json/releases/download/${CLDR_VERSION}/cldr-${CLDR_VERSION}-json-modern.zip")
set(CLDR_ZIP_PATH "${CLDR_PATH}/cldr.zip")

if (ENABLE_UNICODE_DATABASE_DOWNLOAD)
    remove_path_if_version_changed("${CLDR_VERSION}" "${CLDR_VERSION_FILE}" "${CLDR_PATH}")

    if (ENABLE_NETWORK_DOWNLOADS)
        download_file("${CLDR_ZIP_URL}" "${CLDR_ZIP_PATH}" SHA256 "${CLDR_SHA256}")
    else()
        message(STATUS "Skipping download of ${CLDR_ZIP_URL}, expecting the archive to have been extracted to ${CLDR_PATH}")
    endif()
endif()
