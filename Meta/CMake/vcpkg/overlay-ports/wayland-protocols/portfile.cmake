set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        "force-build" FORCE_BUILD
)

if(NOT X_VCPKG_FORCE_VCPKG_WAYLAND_LIBRARIES AND NOT VCPKG_TARGET_IS_WINDOWS AND NOT FORCE_BUILD)
    message(STATUS "Utils and libraries provided by '${PORT}' should be provided by your system! Install the required packages or force vcpkg libraries by setting X_VCPKG_FORCE_VCPKG_WAYLAND_LIBRARIES")
    set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
    return()
endif()


if(NOT FORCE_BUILD OR NOT X_VCPKG_FORCE_VCPKG_WAYLAND_LIBRARIES)
    message(FATAL_ERROR "To build wayland libraries the `force-build` feature must be enabled and the X_VCPKG_FORCE_VCPKG_WAYLAND_LIBRARIES triplet variable must be set.")
endif()

vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.freedesktop.org
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wayland/wayland-protocols
    REF ${VERSION}
    SHA512 3c7a8370498a6ed1767bbe1ad23c97805c1cb524157d88a83ecc79caa511f3fc9ad74bf6124f84e2edeffd5d5d2895a0c68f5100d3ef62d38de44a8e55a12605
    HEAD_REF master
    PATCHES
        cross-build.diff
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dtests=false
    ADDITIONAL_BINARIES
        "wayland_scanner = ['${CURRENT_HOST_INSTALLED_DIR}/tools/wayland/wayland-scanner${VCPKG_HOST_EXECUTABLE_SUFFIX}']"
)
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
