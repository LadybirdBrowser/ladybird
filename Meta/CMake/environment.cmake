if (NOT DEFINED ENV{LADYBIRD_SOURCE_DIR} OR "$ENV{LADYBIRD_SOURCE_DIR}" STREQUAL "")
    set(ENV{LADYBIRD_SOURCE_DIR} "${CMAKE_SOURCE_DIR}")
endif()

if (NOT DEFINED ENV{LADYBIRD_MAIN_SOURCE_DIR} OR "$ENV{LADYBIRD_MAIN_SOURCE_DIR}" STREQUAL "")
    set(ENV{LADYBIRD_MAIN_SOURCE_DIR} "$ENV{LADYBIRD_SOURCE_DIR}")
endif()

set(LADYBIRD_CACHE_DIR "$ENV{LADYBIRD_MAIN_SOURCE_DIR}/Build/caches" CACHE STRING "")

if (DEFINED LADYBIRD_VCPKG_TYPE)
    if (NOT DEFINED ENV{VCPKG_ROOT} OR "$ENV{VCPKG_ROOT}" STREQUAL "")
        set(ENV{VCPKG_ROOT} "$ENV{LADYBIRD_MAIN_SOURCE_DIR}/Build/vcpkg-${LADYBIRD_VCPKG_TYPE}")
    endif()

    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
    set(VCPKG_INSTALL_OPTIONS "--no-print-usage" CACHE STRING "" FORCE)
    if (CMAKE_HOST_APPLE)
        # Installed files are hard links to their copies in packages/, and macOS can resolve @loader_path through either
        # name. From packages/, host tools like Qt's moc can't find the libraries they link.
        set(VCPKG_INSTALL_OPTIONS "${VCPKG_INSTALL_OPTIONS};--clean-packages-after-build" CACHE STRING "" FORCE)
    endif()

    # Each optional dependency is a feature of vcpkg.json that an option selects. The options are declared after
    # project(), so one that is not set yet has its default. Keep in sync with MANIFEST_FEATURE_OPTIONS in
    # Meta/Utils/vcpkg_deps.py.
    if (LADYBIRD_BUILD_QT AND (NOT DEFINED ENABLE_GUI_TARGETS OR ENABLE_GUI_TARGETS))
        list(APPEND VCPKG_MANIFEST_FEATURES "qt")
    endif()
    if ((NOT DEFINED LADYBIRD_ENABLE_UPDATE_CHECK OR LADYBIRD_ENABLE_UPDATE_CHECK)
        AND (NOT DEFINED LADYBIRD_ENABLE_SPARKLE OR LADYBIRD_ENABLE_SPARKLE))
        list(APPEND VCPKG_MANIFEST_FEATURES "sparkle")
    endif()

    set(VCPKG_OVERLAY_TRIPLETS "$ENV{LADYBIRD_SOURCE_DIR}/Meta/CMake/vcpkg/${LADYBIRD_VCPKG_TYPE}-triplets" CACHE STRING "" FORCE)

    set(ENV{VCPKG_BINARY_SOURCES} "clear;files,${LADYBIRD_CACHE_DIR}/vcpkg-binary-cache,readwrite;$ENV{VCPKG_BINARY_SOURCES}")
    set(ENV{X_VCPKG_ASSET_SOURCES} "clear;x-azurl,https://vcpkg-cache.app.ladybird.org/ladybird/source-assets/,$ENV{VCPKG_CACHE_SAS},read$ENV{VCPKG_CACHE_MODE}")
endif()
