include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)

set(CLDR_VERSION "45.0.0")
set(CLDR_SHA256 "ba934cdd40ad4fb6439004c7e746bef97fe2b597db1040fcaa6c7d0647742c1b")

set(CLDR_PATH "${SERENITY_CACHE_DIR}/CLDR" CACHE PATH "Download location for CLDR files")
set(CLDR_VERSION_FILE "${CLDR_PATH}/version.txt")

set(CLDR_ZIP_URL "https://github.com/unicode-org/cldr-json/releases/download/${CLDR_VERSION}/cldr-${CLDR_VERSION}-json-modern.zip")
set(CLDR_ZIP_PATH "${CLDR_PATH}/cldr.zip")

set(CLDR_BCP47_SOURCE cldr-bcp47)
set(CLDR_BCP47_PATH "${CLDR_PATH}/${CLDR_BCP47_SOURCE}")

set(CLDR_CORE_SOURCE cldr-core)
set(CLDR_CORE_PATH "${CLDR_PATH}/${CLDR_CORE_SOURCE}")

set(CLDR_DATES_SOURCE cldr-dates-modern)
set(CLDR_DATES_PATH "${CLDR_PATH}/${CLDR_DATES_SOURCE}")

set(CLDR_NUMBERS_SOURCE cldr-numbers-modern)
set(CLDR_NUMBERS_PATH "${CLDR_PATH}/${CLDR_NUMBERS_SOURCE}")

if (ENABLE_UNICODE_DATABASE_DOWNLOAD)
    remove_path_if_version_changed("${CLDR_VERSION}" "${CLDR_VERSION_FILE}" "${CLDR_PATH}")

    if (ENABLE_NETWORK_DOWNLOADS)
        download_file("${CLDR_ZIP_URL}" "${CLDR_ZIP_PATH}" SHA256 "${CLDR_SHA256}")
        extract_path("${CLDR_PATH}" "${CLDR_ZIP_PATH}" "${CLDR_BCP47_SOURCE}/**" "${CLDR_BCP47_PATH}")
        extract_path("${CLDR_PATH}" "${CLDR_ZIP_PATH}" "${CLDR_CORE_SOURCE}/**" "${CLDR_CORE_PATH}")
        extract_path("${CLDR_PATH}" "${CLDR_ZIP_PATH}" "${CLDR_DATES_SOURCE}/**" "${CLDR_DATES_PATH}")
        extract_path("${CLDR_PATH}" "${CLDR_ZIP_PATH}" "${CLDR_NUMBERS_SOURCE}/**" "${CLDR_NUMBERS_PATH}")
    else()
        message(STATUS "Skipping download of ${CLDR_ZIP_URL}, expecting the archive to have been extracted to ${CLDR_PATH}")
    endif()

    set(LOCALE_DATA_HEADER LocaleData.h)
    set(LOCALE_DATA_IMPLEMENTATION LocaleData.cpp)

    invoke_generator(
        "LocaleData"
        Lagom::GenerateLocaleData
        "${CLDR_VERSION_FILE}"
        "${LOCALE_DATA_HEADER}"
        "${LOCALE_DATA_IMPLEMENTATION}"
        arguments -b "${CLDR_BCP47_PATH}" -r "${CLDR_CORE_PATH}" -n "${CLDR_NUMBERS_PATH}" -d "${CLDR_DATES_PATH}"
    )

    set(LOCALE_DATA_SOURCES
        ${LOCALE_DATA_HEADER}
        ${LOCALE_DATA_IMPLEMENTATION}
    )
endif()
