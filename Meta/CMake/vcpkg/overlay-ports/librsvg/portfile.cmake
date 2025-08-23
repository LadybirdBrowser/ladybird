# port update requires rust/cargo

string(REGEX REPLACE "^([0-9]*[.][0-9]*)[.].*" "\\1" MAJOR_MINOR "${VERSION}")

vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.gnome.org/
    OUT_SOURCE_PATH SOURCE_PATH
    REPO GNOME/librsvg
    REF librsvg-gtk-${VERSION}
    SHA512 1fe06d7e745a53f3aee7b1942f7551c5716ec6abf328fa395006a7aede9f4ef242d604d5f8069c397d86ec3ac095daf49b18b2b34abc67fdcd4a113207fd6a96
    HEAD_REF master # branch name
    PATCHES
        fix-libxml2-2.13.5.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/config.h.linux" DESTINATION "${SOURCE_PATH}")

vcpkg_find_acquire_program(PKGCONFIG)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        "-DGLIB_MKENUMS=${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-mkenums"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    file(GLOB_RECURSE pc_files "${CURRENT_PACKAGES_DIR}/*.pc")
    foreach(pc_file IN LISTS pc_files)
        vcpkg_replace_string("${pc_file}" " -lm" "")
    endforeach()
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(COPY "${CURRENT_PORT_DIR}/unofficial-librsvg-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/unofficial-librsvg")
file(COPY "${CURRENT_PORT_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
