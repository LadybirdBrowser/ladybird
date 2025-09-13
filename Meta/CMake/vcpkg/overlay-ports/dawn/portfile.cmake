if (VCPKG_TARGET_IS_LINUX AND "x11" IN_LIST FEATURES)
    message(WARNING
            [[
dawn support requires the following libraries from the system package manager:

    libxrandr-dev libxinerama-dev libxcursor-dev libx11-xcb-dev mesa-common-dev

They can be installed on Debian based systems via

    apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libx11-xcb-dev mesa-common-dev
]]
    )
endif()

set(ENV{DEPOT_TOOLS_WIN_TOOLCHAIN} 0)
set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)

vcpkg_find_acquire_program(PYTHON3)
get_filename_component(PYTHON3_PATH ${PYTHON3} DIRECTORY)
vcpkg_add_to_path(PREPEND "${PYTHON3_PATH}")

function(dawn_fetch)
    set(oneValueArgs DESTINATION URL REF SOURCE)
    set(multipleValuesArgs PATCHES)
    cmake_parse_arguments(DAWN "" "${oneValueArgs}" "${multipleValuesArgs}" ${ARGN})

    message(STATUS "Fetching ${DAWN_DESTINATION}...")

    if(NOT DEFINED DAWN_DESTINATION)
        message(FATAL_ERROR "DESTINATION must be specified.")
    endif()

    if(NOT DEFINED DAWN_URL)
        message(FATAL_ERROR "The git url must be specified")
    endif()

    if(NOT DEFINED DAWN_REF)
        message(FATAL_ERROR "The git ref must be specified.")
    endif()

    if(EXISTS "${DAWN_SOURCE}/${DAWN_DESTINATION}/.git")
        vcpkg_execute_required_process(
            COMMAND ${GIT} reset --hard
            WORKING_DIRECTORY ${DAWN_SOURCE}/${DAWN_DESTINATION}
            LOGNAME build-${TARGET_TRIPLET})
    else()
        vcpkg_execute_required_process(
            COMMAND ${GIT} clone --depth 1 ${DAWN_URL} ${DAWN_DESTINATION}
            WORKING_DIRECTORY ${DAWN_SOURCE}
            LOGNAME build-${TARGET_TRIPLET})
        vcpkg_execute_required_process(
            COMMAND ${GIT} fetch --depth 1 origin ${DAWN_REF}
            WORKING_DIRECTORY ${DAWN_SOURCE}/${DAWN_DESTINATION}
            LOGNAME build-${TARGET_TRIPLET})
        vcpkg_execute_required_process(
            COMMAND ${GIT} checkout FETCH_HEAD
            WORKING_DIRECTORY ${DAWN_SOURCE}/${DAWN_DESTINATION}
            LOGNAME build-${TARGET_TRIPLET})
    endif()
    foreach(PATCH ${DAWN_PATCHES})
        vcpkg_execute_required_process(
            COMMAND ${GIT} apply ${PATCH}
            WORKING_DIRECTORY ${DAWN_SOURCE}/${DAWN_DESTINATION}
            LOGNAME build-${TARGET_TRIPLET})
    endforeach()
endfunction()

set(DAWN_PATCHES
    remove_partition_alloc_dep.patch # XCode clang was getting libc++ compile errors for partition alloc, given it is an optional dependency and complicates debug builds, lets just disable it on all platforms for now
    disable_opengl_backends.patch # Vulkan and Metal backends are enough for now
)
if(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND DAWN_PATCHES windows_gn_script_py_executable.patch)  # Fixes Windows the following generate failure: ERROR Could not find "python3" from dotfile in PATH.
    list(APPEND DAWN_PATCHES windows_disable_directx_backends.patch)  # We can just use Vulkan on Windows, DirectX would only be required for UWP support
elseif(VCPKG_TARGET_IS_LINUX)
    list(APPEND DAWN_PATCHES linux_disable_x11.patch) # Defaults all X11 config options to false and makes wayland and x11 mutually exclusive
endif()

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://dawn.googlesource.com/dawn
    REF 46b4670bc67cb4f6d34f6ce6a46ba7e1d6059abf
    PATCHES ${DAWN_PATCHES}
)

message(STATUS "Fetching submodules")
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/build
dawn_fetch(
    DESTINATION build
    URL https://chromium.googlesource.com/chromium/src/build.git
    REF db0d31d702840881c049c523a2226e8e391929bf
    SOURCE ${SOURCE_PATH}
    PATCHES "${CMAKE_CURRENT_LIST_DIR}/macos_build_find_sdk.patch" # Extracted ports of https://github.com/microsoft/vcpkg/blame/master/ports/chromium-base/res/0002-build.patch to resolve the configure error "xcode-select: error: tool 'xcodebuild' requires Xcode, but active developer directory '/Library/Developer/CommandLineTools' is a command line tools instance" that occurs in Github Actions CI
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/buildtools
dawn_fetch(
    DESTINATION buildtools
    URL https://chromium.googlesource.com/chromium/src/buildtools.git
    REF 244e7cf4453305d0c17d500662a69fba2e46a73e
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/testing
dawn_fetch(
    DESTINATION testing
    URL https://chromium.googlesource.com/chromium/src/testing.git
    REF ae9705179f821d1dbd2b0a2ba7a6582faac7f86b
    SOURCE ${SOURCE_PATH}
    PATCHES "${CMAKE_CURRENT_LIST_DIR}/testing_remove_catapult_deps.patch" # Prevents us from having to clone the large catapult repository as we don't use it and it also causes issues on Windows during git fetch due to long paths
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/abseil-cpp
dawn_fetch(
    DESTINATION third_party/abseil-cpp
    URL https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp.git
    REF 04dc59d2c83238cb1fcb49083e5e416643a899ce
    SOURCE ${SOURCE_PATH}
)
# We don't actually invoke depot_tools here, but on Windows the configure step fails without this as it queries some content assuming depot_tools exists in this location
if(VCPKG_TARGET_IS_WINDOWS)
    # https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/depot_tools
    dawn_fetch(
        DESTINATION third_party/depot_tools
        URL https://chromium.googlesource.com/chromium/tools/depot_tools.git
        REF d255a8d41e7a2fdc6b50fee69e70014f875d47ef
        SOURCE ${SOURCE_PATH}
    )
endif()
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/jinja2
dawn_fetch(
    DESTINATION third_party/jinja2
    URL https://chromium.googlesource.com/chromium/src/third_party/jinja2.git
    REF e2d024354e11cc6b041b0cff032d73f0c7e43a07
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/libprotobuf-mutator/src
dawn_fetch(
    DESTINATION third_party/libprotobuf-mutator/src
    URL https://chromium.googlesource.com/external/github.com/google/libprotobuf-mutator.git
    REF 7bf98f78a30b067e22420ff699348f084f802e12
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/protobuf
dawn_fetch(
    DESTINATION third_party/protobuf
    URL https://chromium.googlesource.com/chromium/src/third_party/protobuf.git
    REF 1a4051088b71355d44591172c474304331aaddad
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/markupsafe
dawn_fetch(
    DESTINATION third_party/markupsafe
    URL https://chromium.googlesource.com/chromium/src/third_party/markupsafe.git
    REF 0bad08bb207bbfc1d6f3bbc82b9242b0c50e5794
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/glslang/src
dawn_fetch(
    DESTINATION third_party/glslang/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/glslang.git
    REF 21b4e37133868b3a50ef15fc027ecd6d3a52c875
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/spirv-headers/src
dawn_fetch(
    DESTINATION third_party/spirv-headers/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Headers.git
    REF 2a611a970fdbc41ac2e3e328802aed9985352dca
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/spirv-tools/src
dawn_fetch(
    DESTINATION third_party/spirv-tools/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Tools.git
    REF 108b19e5c6979f496deffad4acbe354237afa7d3
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/vulkan-headers/src
dawn_fetch(
    DESTINATION third_party/vulkan-headers/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Headers.git
    REF 10739e8e00a7b6f74d22dd0a547f1406ff1f5eb9
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/vulkan-loader/src
dawn_fetch(
    DESTINATION third_party/vulkan-loader/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader.git
    REF c8a2c8c9164a58ce71c1c77104e28e8de724539e
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/vulkan-utility-libraries/src
dawn_fetch(
    DESTINATION third_party/vulkan-utility-libraries/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Utility-Libraries.git
    REF 72665ee1e50db3d949080df8d727dffa8067f5f8
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/vulkan-validation-layers/src
dawn_fetch(
    DESTINATION third_party/vulkan-validation-layers/src
    URL https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-ValidationLayers.git
    REF 69e6081acb3d2f55cb98ae1d5faa17b6db2044c7
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/third_party/webgpu-headers/src
dawn_fetch(
    DESTINATION third_party/webgpu-headers/src
    URL https://chromium.googlesource.com/external/github.com/webgpu-native/webgpu-headers.git
    REF 4f617851dfa20bd240436d9255bcb7e4dbbb1e3f
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/tools/clang
# The official supported compiler is Clang 19+, so we will use clang on all platforms to build.
# I did try to build dawn with GCC on Linux, but it seems to have quite a few build issues which isn't surprising given https://chromium.googlesource.com/chromium/src/+/refs/tags/139.0.7258.88/docs/clang.md#using-gcc-on-linux.
dawn_fetch(
    DESTINATION tools/clang
    URL https://chromium.googlesource.com/chromium/src/tools/clang.git
    REF 5d9b09742311e059ecdba6d74adcb883e4ebffe5
    SOURCE ${SOURCE_PATH}
)
# https://dawn.googlesource.com/dawn/+/refs/heads/chromium/7258/tools/protoc_wrapper
dawn_fetch(
    DESTINATION tools/protoc_wrapper
    URL https://chromium.googlesource.com/chromium/src/tools/protoc_wrapper.git
    REF 8ad6d21544b14c7f753852328d71861b363cc512
    SOURCE ${SOURCE_PATH}
)

# Required because gn requires these files but they are only added automatically when using depot_tools (the v8 port does this as well)
file(WRITE "${SOURCE_PATH}/build/config/gclient_args.gni" "checkout_google_benchmark = false\ngenerate_location_tags = false\n")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/LASTCHANGE.committime" DESTINATION "${SOURCE_PATH}/build/util")

# We need to run this before configure to download the bundled clang gn expects to exist
vcpkg_execute_required_process(
    COMMAND ${PYTHON3} tools/clang/scripts/update.py
    WORKING_DIRECTORY ${SOURCE_PATH}
    LOGNAME build-${TARGET_TRIPLET}
)

set(DAWN_USE_WAYLAND "dawn_use_wayland=false")
set(DAWN_USE_X11 "dawn_use_x11=false")
if("x11" IN_LIST FEATURES)
    set(DAWN_USE_X11 "dawn_use_x11=true")
endif()
if("wayland" IN_LIST FEATURES)
    set(DAWN_USE_WAYLAND "dawn_use_wayland=true")
endif()

set(DAWN_ENABLE_VULKAN "dawn_enable_vulkan=false")
set(DAWN_ENABLE_METAL "dawn_enable_metal=false")
if("vulkan" IN_LIST FEATURES)
    set(DAWN_ENABLE_VULKAN "dawn_enable_vulkan=true")
endif()
if("metal" IN_LIST FEATURES)
    set(DAWN_ENABLE_METAL "dawn_enable_metal=true")
endif()

# Vulkan and Metal backends are enough for now, DirectX would only be required for UWP support
set(DAWN_ENABLE_D3D11 "dawn_enable_d3d11=false")
set(DAWN_ENABLE_D3D12 "dawn_enable_d3d12=false")
set(DAWN_ENABLE_DESKTOP_GL "dawn_enable_desktop_gl=false")
set(DAWN_ENABLE_OPENGLES "dawn_enable_opengles=false")

set(DAWN_FORCE_DYNAMIC_CRT)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    set(DAWN_COMPLETE_STATIC_LIBS "dawn_complete_static_libs=true")
    if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_CRT_LINKAGE STREQUAL "dynamic")
        set(DAWN_FORCE_DYNAMIC_CRT "force_dynamic_crt=true")
    endif()

    set(DAWN_PROC_TARGET "src/dawn:proc_static")
    set(DAWN_PROC_LIB "dawn_proc_static")

    set(DAWN_NATIVE_TARGET "src/dawn/native:static")
    set(DAWN_NATIVE_LIB "dawn_native_static")

    set(DAWN_PLATFORM_TARGET "src/dawn/platform:static")
    set(DAWN_PLATFORM_LIB "dawn_platform_static")

    set(DAWN_WEBGPU_TARGET "src/dawn/native:webgpu_dawn_static")
    set(DAWN_WEBGPU_LIB "webgpu_dawn_static")
else()
    set(DAWN_COMPLETE_STATIC_LIBS)

    set(DAWN_PROC_TARGET "src/dawn:proc_shared")
    set(DAWN_PROC_LIB "dawn_proc")

    set(DAWN_NATIVE_TARGET "src/dawn/native:shared")
    set(DAWN_NATIVE_LIB "dawn_native")

    set(DAWN_PLATFORM_TARGET "src/dawn/platform:shared")
    set(DAWN_PLATFORM_LIB "dawn_platform")

    set(DAWN_WEBGPU_TARGET "src/dawn/native:webgpu_dawn_shared")
    set(DAWN_WEBGPU_LIB "webgpu_dawn")
endif()

vcpkg_gn_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS "target_cpu=\"${VCPKG_TARGET_ARCHITECTURE}\" use_sysroot=false ${DAWN_FORCE_DYNAMIC_CRT} ${DAWN_COMPLETE_STATIC_LIBS} tint_build_hlsl_writer=false tint_has_fuzzers=false tint_build_unittests=false tint_build_benchmarks=false is_clang=true use_glib=false use_custom_libcxx=false dawn_standalone=true ${DAWN_USE_X11} ${DAWN_USE_WAYLAND} dawn_use_swiftshader=false dawn_tests_use_angle=false ${DAWN_ENABLE_VULKAN} ${DAWN_ENABLE_METAL} ${DAWN_ENABLE_D3D11} ${DAWN_ENABLE_D3D12} ${DAWN_ENABLE_DESKTOP_GL} ${DAWN_ENABLE_OPENGLES}"
    OPTIONS_DEBUG "is_debug=true"
    OPTIONS_RELEASE "is_debug=false"
)

vcpkg_gn_install(
    SOURCE_PATH "${SOURCE_PATH}"
    TARGETS ${DAWN_PROC_TARGET} ${DAWN_NATIVE_TARGET} ${DAWN_PLATFORM_TARGET} ${DAWN_WEBGPU_TARGET}
)

file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include" FILES_MATCHING PATTERN "*.h")

# The generated header targets do exist; however, vcpkg_gn_install installs them to the lib directory, so we need to manually install them to the include directory
if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
    set(DAWN_GENERATED_HEADERS_DIR_PREFIX "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg")
endif()
if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
    set(DAWN_GENERATED_HEADERS_DIR_PREFIX "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel")
endif()
set(DAWN_GENERATED_HEADERS_DIR "${DAWN_GENERATED_HEADERS_DIR_PREFIX}/gen/include/dawn")
set(WEBGPU_GENERATED_HEADERS_DIR "${DAWN_GENERATED_HEADERS_DIR_PREFIX}/gen/include/webgpu")

# These are the headers output by the following commands:
#   - `gn desc <build_dir> include/dawn:headers_gen outputs`
#   - `gn desc <build_dir> include/dawn:cpp_headers_gen outputs`
set(DAWN_GENERATED_HEADER_FILES
    "${DAWN_GENERATED_HEADERS_DIR}/dawn_proc_table.h"
    "${DAWN_GENERATED_HEADERS_DIR}/webgpu.h"
    "${DAWN_GENERATED_HEADERS_DIR}/webgpu_cpp.h"
    "${DAWN_GENERATED_HEADERS_DIR}/webgpu_cpp_print.h"
)
set(WEBGPU_GENERATED_HEADER_FILES
    "${WEBGPU_GENERATED_HEADERS_DIR}/webgpu_cpp_chained_struct.h"
)
# Unfortunately some convoluted manual header organization is required here to put things in the right place for consumers that `#include <webgpu/webgpu_cpp.h>`
file(INSTALL ${WEBGPU_GENERATED_HEADERS_DIR} DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL ${DAWN_GENERATED_HEADER_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/include/dawn")
file(INSTALL ${DAWN_GENERATED_HEADER_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/include/webgpu/dawn")

function(install_pc_file name pc_data)
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        configure_file("${CMAKE_CURRENT_LIST_DIR}/dawn.pc.in" "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${name}.pc" @ONLY)
    endif()
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        configure_file("${CMAKE_CURRENT_LIST_DIR}/dawn.pc.in" "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/${name}.pc" @ONLY)
    endif()
endfunction()

install_pc_file(unofficial_webgpu_dawn [[
Name:
Description: Dawn WebGPU implementation library
Libs: -L"${libdir}" -ldawn_proc -ldawn_native -ldawn_platform -lwebgpu_dawn
Cflags: -I"${includedir}"
]])

vcpkg_fixup_pkgconfig()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
