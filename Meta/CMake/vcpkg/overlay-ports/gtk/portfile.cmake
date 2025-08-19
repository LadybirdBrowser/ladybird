# It installs only shared libs, regardless build type.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.gnome.org/
    OUT_SOURCE_PATH SOURCE_PATH
    REPO GNOME/gtk
    REF ${VERSION}
    SHA512 73397bbc1f6bb9fd0fa2a8eedd6247cf3e4b70fb93ee62512a4bb6b205f4702d2e2680b655d7e06908f9d911442f07e29b7af2567d1806b63f0a13ca0f9f5572
    HEAD_REF master # branch name
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

list(APPEND OPTIONS -Dx11-backend=${x11}) #Enable the X11 gdk backend (only when building on Unix)
list(APPEND OPTIONS -Dwayland-backend=${wayland}) # Enable the Wayland gdk backend (only when building on Unix except for macOS)
list(APPEND OPTIONS -Dbroadway-backend=false) #Enable the broadway (HTML5) gdk backend
list(APPEND OPTIONS -Dwin32-backend=${win32}) #Enable the Windows gdk backend (only when building on Windows)
list(APPEND OPTIONS -Dmacos-backend=${osx}) #Enable the macOS gdk backend (only when building on macOS)
list(APPEND OPTIONS -Dvulkan=${vulkan}) #Enable the Vulkan support (only when building on Unix)

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
        -Dmedia-gstreamer=disabled  # Build the gstreamer media backend
        -Dprint-cups=disabled       # Build the cups print backend
        -Dcloudproviders=disabled   # Enable the cloudproviders support
        -Dsysprof=disabled          # include tracing support for sysprof
        -Dtracker=disabled          # Enable Tracker3 filechooser search
        -Dcolord=disabled           # Build colord support for the CUPS printing backend
        -Df16c=disabled             # Enable F16C fast paths (requires F16C)
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
