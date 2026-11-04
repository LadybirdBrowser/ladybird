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

    # The vcpkg toolchain caches the root it finds next to itself, and never looks again while that's cached. So a build
    # tree configured when the root was somewhere else (e.g. the Build/vcpkg checkout every preset used to share) would
    # keep reading that checkout's version database, and running its vcpkg. Drop a cached root that isn't this one, so
    # the toolchain finds the root again.
    if (DEFINED CACHE{Z_VCPKG_ROOT_DIR})
        file(REAL_PATH "$CACHE{Z_VCPKG_ROOT_DIR}" cached_vcpkg_root)
        file(REAL_PATH "$ENV{VCPKG_ROOT}" vcpkg_root)
        if (NOT cached_vcpkg_root STREQUAL vcpkg_root)
            message(STATUS "Using the vcpkg root ${vcpkg_root}, rather than the cached ${cached_vcpkg_root}")
            unset(Z_VCPKG_ROOT_DIR CACHE)
        endif()
    endif()
    set(VCPKG_INSTALL_OPTIONS "--no-print-usage" CACHE STRING "" FORCE)
    if (CMAKE_HOST_APPLE)
        # Installed files are hard links to their copies in packages/, and macOS can resolve @loader_path through either
        # name. From packages/, host tools like Qt's moc can't find the libraries they link.
        set(VCPKG_INSTALL_OPTIONS "${VCPKG_INSTALL_OPTIONS};--clean-packages-after-build" CACHE STRING "" FORCE)
    endif()
    set(VCPKG_OVERLAY_TRIPLETS "$ENV{LADYBIRD_SOURCE_DIR}/Meta/CMake/vcpkg/${LADYBIRD_VCPKG_TYPE}-triplets" CACHE STRING "" FORCE)

    set(ENV{VCPKG_BINARY_SOURCES} "clear;files,${LADYBIRD_CACHE_DIR}/vcpkg-binary-cache,readwrite;$ENV{VCPKG_BINARY_SOURCES}")
    set(ENV{X_VCPKG_ASSET_SOURCES} "clear;x-azurl,https://vcpkg-cache.app.ladybird.org/ladybird/source-assets/,$ENV{VCPKG_CACHE_SAS},read$ENV{VCPKG_CACHE_MODE}")
endif()
