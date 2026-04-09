# It installs only shared libs, regardless build type.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

string(REGEX MATCH [[^[0-9][0-9]*\.[1-9][0-9]*]] VERSION_MAJOR_MINOR "${VERSION}")
vcpkg_download_distfile(ARCHIVE
    URLS
        "https://download.gnome.org/sources/${PORT}/${VERSION_MAJOR_MINOR}/${PORT}-${VERSION}.tar.xz"
        "https://www.mirrorservice.org/sites/ftp.gnome.org/pub/GNOME/sources/${PORT}/${VERSION_MAJOR_MINOR}/${PORT}-${VERSION}.tar.xz"
    FILENAME "GNOME-${PORT}-${VERSION}.tar.xz"
    SHA512 bfea8825c5cdaaaea95055bf522c384b23bb26e9b8205081d055f85dc5df5d357827139115a4b7efe4d42418714b2ac062349459f761b383bed170354ecfa7f4
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        0001-build.patch
)

vcpkg_find_acquire_program(PKGCONFIG)
get_filename_component(PKGCONFIG_DIR "${PKGCONFIG}" DIRECTORY )
vcpkg_add_to_path("${PKGCONFIG_DIR}") # Post install script runs pkg-config so it needs to be on PATH
vcpkg_add_to_path("${CURRENT_HOST_INSTALLED_DIR}/tools/glib/")

set(x11 false)
set(wayland false)
set(vulkan disabled)
set(win32 false)
set(osx false)
if(VCPKG_TARGET_IS_LINUX)
    set(wayland true)
    set(vulkan enabled)
elseif(VCPKG_TARGET_IS_WINDOWS)
    set(win32 true)
elseif(VCPKG_TARGET_IS_OSX)
    set(osx true)
endif()

list(APPEND OPTIONS -Dx11-backend=${x11})
list(APPEND OPTIONS -Dwayland-backend=${wayland})
list(APPEND OPTIONS -Dbroadway-backend=false)
list(APPEND OPTIONS -Dwin32-backend=${win32})
list(APPEND OPTIONS -Dmacos-backend=${osx})
list(APPEND OPTIONS -Dvulkan=${vulkan})

if("introspection" IN_LIST FEATURES)
    list(APPEND OPTIONS_RELEASE -Dintrospection=enabled)
    vcpkg_get_gobject_introspection_programs(PYTHON3 GIR_COMPILER GIR_SCANNER)
else()
    list(APPEND OPTIONS_RELEASE -Dintrospection=disabled)
endif()

vcpkg_configure_meson(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        ${OPTIONS}
        -Dbuild-demos=false
        -Dbuild-testsuite=false
        -Dbuild-examples=false
        -Dbuild-tests=false
        -Ddocumentation=false
        -Dman-pages=false
        -Dmedia-gstreamer=disabled
        -Dprint-cups=disabled
        -Dcloudproviders=disabled
        -Dsysprof=disabled
        -Dtracker=disabled
        -Dcolord=disabled
        -Df16c=disabled
    OPTIONS_RELEASE
        ${OPTIONS_RELEASE}
    OPTIONS_DEBUG
        -Dintrospection=disabled
    ADDITIONAL_BINARIES
        glib-genmarshal='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-genmarshal'
        glib-mkenums='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-mkenums'
        glib-compile-resources='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-compile-resources${VCPKG_HOST_EXECUTABLE_SUFFIX}'
        gdbus-codegen='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/gdbus-codegen'
        glib-compile-schemas='${CURRENT_HOST_INSTALLED_DIR}/tools/glib/glib-compile-schemas${VCPKG_HOST_EXECUTABLE_SUFFIX}'
        sassc='${CURRENT_HOST_INSTALLED_DIR}/tools/sassc/bin/sassc${VCPKG_HOST_EXECUTABLE_SUFFIX}'
        "g-ir-compiler='${GIR_COMPILER}'"
        "g-ir-scanner='${GIR_SCANNER}'"
        glslc='${CURRENT_HOST_INSTALLED_DIR}/tools/shaderc/glslc'
)

vcpkg_install_meson(ADD_BIN_TO_PATH)

vcpkg_copy_pdbs()

vcpkg_fixup_pkgconfig()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")

set(TOOL_NAMES gtk4-builder-tool
               gtk4-encode-symbolic-svg
               gtk4-path-tool
               gtk4-query-settings
               gtk4-rendernode-tool
               gtk4-update-icon-cache
               gtk4-image-tool)
if(VCPKG_TARGET_IS_LINUX)
    list(APPEND TOOL_NAMES gtk4-launch)
endif()
vcpkg_copy_tools(TOOL_NAMES ${TOOL_NAMES} AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()
