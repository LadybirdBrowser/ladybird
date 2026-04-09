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
    SHA512 39136e93626955f49f0906d5bcd0ec95ff337a612f2ce3e63196ca3a55bf91682e1a3da63c43b066d504a3851aebce78dc34a64c5f21c76446b00405cf870378
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
