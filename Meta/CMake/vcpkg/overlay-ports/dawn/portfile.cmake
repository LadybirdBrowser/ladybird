if (VCPKG_TARGET_IS_EMSCRIPTEN)
    vcpkg_download_distfile(ARCHIVE
        URLS "https://github.com/google/dawn/releases/download/v${VERSION}/emdawnwebgpu_pkg-v${VERSION}.zip"
        FILENAME "emdawnwebgpu_pkg-v${VERSION}.zip"
        SHA512 a33cd20bde55eb7b4dd62843ebdc7125074fe046b8a2a9b37e9ef9bffe5bd03856749c6de4dcf3c3b7fef98e47f0c69addd6a6a1016c00725f8457a42e107b8c
    )
    vcpkg_extract_source_archive(
        SOURCE_PATH
        ARCHIVE ${ARCHIVE}
        PATCHES
            000-fix-emdawnwebgpu.patch
    )
    set(VCPKG_BUILD_TYPE release)
    file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/DawnConfig.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
    file(INSTALL "${SOURCE_PATH}/webgpu/include" DESTINATION "${CURRENT_PACKAGES_DIR}")
    file(INSTALL "${SOURCE_PATH}/webgpu_cpp/include" DESTINATION "${CURRENT_PACKAGES_DIR}")
    file(INSTALL "${SOURCE_PATH}/webgpu/src" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" PATTERN "LICENSE" EXCLUDE)
    file(INSTALL "${SOURCE_PATH}/emdawnwebgpu.port.py" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
    set(DAWN_PKGCONFIG_CFLAGS "--use-port=\${prefix}/share/${PORT}/emdawnwebgpu.port.py")
    set(DAWN_PKGCONFIG_LIBS "--use-port=\${prefix}/share/${PORT}/emdawnwebgpu.port.py")
    set(DAWN_PKGCONFIG_REQUIRES "")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/unofficial_webgpu_dawn.pc.in" "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/unofficial_webgpu_dawn.pc" @ONLY)
    vcpkg_fixup_pkgconfig()
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/webgpu/src/LICENSE" "${SOURCE_PATH}/webgpu_cpp/LICENSE")
    file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
    return()
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/dawn
    REF "v${VERSION}"
    SHA512 8726185122f23084db5358c4141d95e8f32f40e4121671422af9c9d2c984ebc9a806edf4f32db65398d414b94b93e48041fd75bbe5afd2582dc2155f6ddea7ad
    HEAD_REF master
    PATCHES
        001-fix-windows-build.patch
        002-fix-uwp.patch
        003-fix-d3d11.patch
        004-deps.patch
)

# vcpkg_find_acquire_program(PYTHON3)
# vcpkg_execute_in_download_mode(
#     COMMAND "${PYTHON3}" tools/fetch_dawn_dependencies.py
#     WORKING_DIRECTORY "${SOURCE_PATH}"
# )
#
# get_dawn_deps_commit() { curl -s "https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7371/$1" | htmlq .gitlink-detail --text; }
#

function(checkout_in_path PATH URL REF)
    if(EXISTS "${PATH}")
        file(GLOB_RECURSE subdirectory_children "${CURRENT_PACKAGES_DIR}/include/${directory_child}/*")
        if(NOT "${subdirectory_children}" STREQUAL "")
            return()
        else()
            file(REMOVE_RECURSE "${PATH}")
        endif()
    endif()

    vcpkg_from_git(
        OUT_SOURCE_PATH DEP_SOURCE_PATH
        URL "${URL}"
        REF "${REF}"
    )
    file(RENAME "${DEP_SOURCE_PATH}" "${PATH}")
    file(REMOVE_RECURSE "${DEP_SOURCE_PATH}")
endfunction()

checkout_in_path(
    "${SOURCE_PATH}/third_party/jinja2"
    "https://chromium.googlesource.com/chromium/src/third_party/jinja2"
    "e2d024354e11cc6b041b0cff032d73f0c7e43a07"
)

checkout_in_path(
    "${SOURCE_PATH}/third_party/markupsafe"
    "https://chromium.googlesource.com/chromium/src/third_party/markupsafe"
    "0bad08bb207bbfc1d6f3bbc82b9242b0c50e5794"
)

checkout_in_path(
    "${SOURCE_PATH}/third_party/spirv-headers/src"
    "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Headers"
    "a8637796c28386c3cf3b4e8107020fbb52c46f3f"
)

checkout_in_path(
    "${SOURCE_PATH}/third_party/spirv-tools/src"
    "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Tools"
    "8a8bb6c89174ed753eb18a438092ee59356efc3c"
)

vcpkg_find_acquire_program(PYTHON3)

if (VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    set(DAWN_BUILD_MONOLITHIC_LIBRARY "STATIC")
else()
    set(DAWN_BUILD_MONOLITHIC_LIBRARY "SHARED")
endif()

# DAWN_BUILD_MONOLITHIC_LIBRARY SHARED/STATIC requires BUILD_SHARED_LIBS=OFF
set(VCPKG_LIBRARY_LINKAGE_BACKUP ${VCPKG_LIBRARY_LINKAGE})
set(VCPKG_LIBRARY_LINKAGE static)

set(DAWN_ENABLE_NULL ON)
set(DAWN_ENABLE_D3D11 OFF)
if("d3d11" IN_LIST FEATURES)
    set(DAWN_ENABLE_D3D11 ON)
endif()
set(DAWN_ENABLE_D3D12 OFF)
if("d3d12" IN_LIST FEATURES)
    set(DAWN_ENABLE_D3D12 ON)
endif()
set(DAWN_ENABLE_DESKTOP_GL OFF)
if("gl" IN_LIST FEATURES)
    set(DAWN_ENABLE_DESKTOP_GL ON)
endif()
set(DAWN_ENABLE_OPENGLES OFF)
if("gles" IN_LIST FEATURES)
    set(DAWN_ENABLE_OPENGLES ON)
endif()
set(DAWN_ENABLE_METAL OFF)
if("metal" IN_LIST FEATURES)
    set(DAWN_ENABLE_METAL ON)
endif()
set(DAWN_ENABLE_VULKAN OFF)
if("vulkan" IN_LIST FEATURES)
    set(DAWN_ENABLE_VULKAN ON)
endif()
set(DAWN_USE_WAYLAND OFF)
if("wayland" IN_LIST FEATURES)
    set(DAWN_USE_WAYLAND ON)
endif()
set(DAWN_USE_X11 OFF)
if("x11" IN_LIST FEATURES)
    set(DAWN_USE_X11 ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DPython3_EXECUTABLE="${PYTHON3}"
        -DDAWN_BUILD_MONOLITHIC_LIBRARY=${DAWN_BUILD_MONOLITHIC_LIBRARY}
        -DDAWN_ENABLE_INSTALL=ON
        -DDAWN_USE_GLFW=OFF
        -DDAWN_BUILD_PROTOBUF=OFF
        -DDAWN_BUILD_SAMPLES=OFF
        -DDAWN_BUILD_TESTS=OFF
        -DTINT_BUILD_TESTS=OFF
        -DTINT_ENABLE_INSTALL=OFF
        -DTINT_BUILD_CMD_TOOLS=OFF
        -DDAWN_ENABLE_NULL=${DAWN_ENABLE_NULL}
        -DDAWN_ENABLE_D3D11=${DAWN_ENABLE_D3D11}
        -DDAWN_ENABLE_D3D12=${DAWN_ENABLE_D3D12}
        -DDAWN_ENABLE_DESKTOP_GL=${DAWN_ENABLE_DESKTOP_GL}
        -DDAWN_ENABLE_OPENGLES=${DAWN_ENABLE_OPENGLES}
        -DDAWN_ENABLE_METAL=${DAWN_ENABLE_METAL}
        -DDAWN_ENABLE_VULKAN=${DAWN_ENABLE_VULKAN}
        -DDAWN_USE_WAYLAND=${DAWN_USE_WAYLAND}
        -DDAWN_USE_X11=${DAWN_USE_X11}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/Dawn)

list(APPEND DAWN_ABSL_REQUIRES
    absl_flat_hash_set
    absl_flat_hash_map
    absl_inlined_vector
    absl_no_destructor
    absl_overload
    absl_str_format_internal
    absl_strings
    absl_span
    absl_string_view
)
list(JOIN DAWN_ABSL_REQUIRES ", " DAWN_ABSL_REQUIRES)

set(DAWN_PKGCONFIG_CFLAGS "")
set(DAWN_PKGCONFIG_REQUIRES "${DAWN_ABSL_REQUIRES}")
set(DAWN_PKGCONFIG_LIBS "-lwebgpu_dawn")

if (VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW AND NOT VCPKG_TARGET_IS_UWP)
    set(DAWN_PKGCONFIG_LIBS "${DAWN_PKGCONFIG_LIBS} -lonecore -luser32 -ldelayimp")
endif()
if (DAWN_ENABLE_D3D11 OR DAWN_ENABLE_D3D12)
    set(DAWN_PKGCONFIG_LIBS "${DAWN_PKGCONFIG_LIBS} -ldxguid")
endif()
if (DAWN_ENABLE_METAL)
    set(DAWN_PKGCONFIG_LIBS "${DAWN_PKGCONFIG_LIBS} -framework IOSurface -framework Metal -framework QuartzCore")
    if (VCPKG_TARGET_IS_OSX)
        set(DAWN_PKGCONFIG_LIBS "${DAWN_PKGCONFIG_LIBS} -framework Cocoa -framework IOKit")
    endif()
endif()

if (EXISTS "${CURRENT_PACKAGES_DIR}/debug/lib")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/unofficial_webgpu_dawn.pc.in" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/unofficial_webgpu_dawn.pc" @ONLY)
endif()
if (EXISTS "${CURRENT_PACKAGES_DIR}/lib")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/unofficial_webgpu_dawn.pc.in" "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/unofficial_webgpu_dawn.pc" @ONLY)
endif()
vcpkg_fixup_pkgconfig()

# Restore the original library linkage
set(VCPKG_LIBRARY_LINKAGE ${VCPKG_LIBRARY_LINKAGE_BACKUP})

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
