include("${CMAKE_CURRENT_LIST_DIR}/skia-functions.cmake")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/skia
    REF "ee20d565acb08dece4a32e3f209cdd41119015ca"
    SHA512 AFA54CB14B76E9B2F40498C5DAF93CEC951D12D7F3487DEBA6F8F94A66D046A5625561E1CCD65F2E9EF5E2FC1A3158C3DA4FB9658BCF268248A34C49DEA90D3A
    PATCHES
        # disable-dev-test.patch
        skia-include-string.patch
        bentleyottmann-build.patch
        graphite.patch
        vulkan-headers.patch
        pdfsubsetfont-uwp.diff
        skparagraph-dllexport.patch
        use-pkgconfig-to-find-gl.patch
        dont-use-response-file.patch
        fix-bsd.patch
        allow-disabling-lib-dl.patch
        always-build-pathops.patch
        skstrendswith-dllexport.patch
        skpath-enable-edit-methods.patch # See SkPath section in https://github.com/google/skia/blob/chrome/m143/RELEASE_NOTES.md
        dawn.patch
        skcms-dllexport.patch
        ladybird-disable-skia-managed-resource-trace.patch # Ladybird-specific: disable Ganesh refcount asserts on debug process exit
)

# De-vendor
file(REMOVE_RECURSE "${SOURCE_PATH}/include/third_party/vulkan")
file(REMOVE_RECURSE "${SOURCE_PATH}/third_party/dawn")

# these following aren't available in vcpkg
# to update, visit the DEPS file in Skia's root directory
declare_external_from_git(d3d12allocator
    URL "https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git"
    REF "169895d529dfce00390a20e69c2f516066fe7a3b"
    LICENSE_FILE LICENSE.txt
)
declare_external_from_git(dng_sdk
    URL "https://android.googlesource.com/platform/external/dng_sdk.git"
    REF "dbe0a676450d9b8c71bf00688bb306409b779e90"
    LICENSE_FILE LICENSE
)
declare_external_from_git(jinja2
    URL "https://chromium.googlesource.com/chromium/src/third_party/jinja2"
    REF "c3027d884967773057bf74b957e3fea87e5df4d7"
    LICENSE_FILE LICENSE.rst
)
declare_external_from_git(markupsafe
    URL "https://chromium.googlesource.com/chromium/src/third_party/markupsafe"
    REF "4256084ae14175d38a3ff7d739dca83ae49ccec6"
    LICENSE_FILE LICENSE
)
declare_external_from_git(partition_alloc
    URL "https://chromium.googlesource.com/chromium/src/base/allocator/partition_allocator.git"
    REF "ce13777cb731e0a60c606d1741091fd11a0574d7"
    LICENSE_FILE LICENSE
)
declare_external_from_git(piex
    URL "https://android.googlesource.com/platform/external/piex.git"
    REF "bb217acdca1cc0c16b704669dd6f91a1b509c406"
    LICENSE_FILE LICENSE
)
declare_external_from_git(spirv-cross
    URL "https://github.com/KhronosGroup/SPIRV-Cross"
    REF "b8fcf307f1f347089e3c46eb4451d27f32ebc8d3"
    LICENSE_FILE LICENSE
)
declare_external_from_git(spirv-headers
    URL "https://github.com/KhronosGroup/SPIRV-Headers.git"
    REF "b824a462d4256d720bebb40e78b9eb8f78bbb305"
    LICENSE_FILE LICENSE
)
declare_external_from_git(spirv-tools
    URL "https://github.com/KhronosGroup/SPIRV-Tools.git"
    REF "f410b3c178740f9f5bd28d5b22a71d4bc10acd49"
    LICENSE_FILE LICENSE
)
declare_external_from_git(wuffs
    URL "https://github.com/google/wuffs-mirror-release-c.git"
    REF "e3f919ccfe3ef542cfc983a82146070258fb57f8"
    LICENSE_FILE LICENSE
)

declare_external_from_pkgconfig(abseil-cpp)
declare_external_from_pkgconfig(expat)
declare_external_from_pkgconfig(fontconfig PATH "third_party")
declare_external_from_pkgconfig(freetype2)
declare_external_from_pkgconfig(gl)
declare_external_from_pkgconfig(harfbuzz MODULES harfbuzz harfbuzz-subset)
declare_external_from_pkgconfig(icu MODULES icu-uc)
declare_external_from_pkgconfig(libavif)
declare_external_from_pkgconfig(libjpeg PATH "third_party/libjpeg-turbo" MODULES libturbojpeg libjpeg)
declare_external_from_pkgconfig(libpng)
declare_external_from_pkgconfig(libwebp MODULES libwebpdecoder libwebpdemux libwebpmux libwebp)
declare_external_from_pkgconfig(zlib)

declare_external_from_vcpkg(vulkan_headers PATH third_party/externals/vulkan-headers)

set(known_cpus x86 x64 arm arm64 wasm)
if(NOT VCPKG_TARGET_ARCHITECTURE IN_LIST known_cpus)
    message(WARNING "Unknown target cpu '${VCPKG_TARGET_ARCHITECTURE}'.")
endif()

string(JOIN " " OPTIONS
    "target_cpu=\"${VCPKG_TARGET_ARCHITECTURE}\""
    skia_enable_android_utils=false
    skia_enable_spirv_validation=false
    skia_enable_tools=false
    skia_enable_gpu_debug_layers=false
    skia_use_jpeg_gainmaps=false
    skia_use_lua=false
)
set(OPTIONS_DBG "is_debug=true")
set(OPTIONS_REL "is_official_build=true")
vcpkg_list(SET SKIA_TARGETS :skia :modules)

if(VCPKG_TARGET_IS_ANDROID)
    string(APPEND OPTIONS " target_os=\"android\"")
elseif(VCPKG_TARGET_IS_IOS)
    string(APPEND OPTIONS " target_os=\"ios\"")
elseif(VCPKG_TARGET_IS_EMSCRIPTEN)
    string(APPEND OPTIONS " target_os=\"wasm\"")
elseif(VCPKG_TARGET_IS_WINDOWS)
    string(APPEND OPTIONS " target_os=\"win\"")
    if(VCPKG_TARGET_IS_UWP)
        string(APPEND OPTIONS " skia_enable_winuwp=true skia_enable_fontmgr_win=false skia_use_xps=false")
    endif()
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        string(APPEND OPTIONS " skia_enable_bentleyottmann=false")
    endif()
elseif(VCPKG_TARGET_IS_LINUX OR VCPKG_TARGET_IS_BSD)
    string(APPEND OPTIONS " target_os=\"linux\"")
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    string(APPEND OPTIONS " is_component_build=true")
else()
    string(APPEND OPTIONS " is_component_build=false")
endif()

if (VCPKG_TARGET_IS_BSD AND NOT VCPKG_TARGET_IS_FREEBSD)
    string(APPEND OPTIONS " skia_vcpkg_has_lib_dl=false")
else()
    string(APPEND OPTIONS " skia_vcpkg_has_lib_dl=true")
endif()

set(required_externals
    expat
    piex
    zlib
    wuffs
)

if("dng" IN_LIST FEATURES)
    list(APPEND required_externals dng_sdk)
    string(APPEND OPTIONS " skia_use_dng_sdk=true")
else()
    string(APPEND OPTIONS " skia_use_dng_sdk=false")
endif()

if("fontconfig" IN_LIST FEATURES)
    list(APPEND required_externals fontconfig)
    string(APPEND OPTIONS " skia_use_fontconfig=true")
    if(VCPKG_TARGET_IS_WINDOWS)
        string(APPEND OPTIONS " skia_enable_fontmgr_FontConfigInterface=false")
    endif()
else()
    string(APPEND OPTIONS " skia_use_fontconfig=false")
endif()

if("freetype" IN_LIST FEATURES)
    list(APPEND required_externals freetype2)
    string(APPEND OPTIONS " skia_use_freetype=true")
else()
    string(APPEND OPTIONS " skia_use_freetype=false")
endif()

if("harfbuzz" IN_LIST FEATURES)
    list(APPEND required_externals harfbuzz)
    string(APPEND OPTIONS " skia_use_harfbuzz=true")
else()
    string(APPEND OPTIONS " skia_use_harfbuzz=false")
endif()

if("icu" IN_LIST FEATURES)
    list(APPEND required_externals icu)
    string(APPEND OPTIONS " skia_use_icu=true skia_use_system_icu=true")
else()
    string(APPEND OPTIONS " skia_use_icu=false")
endif()

if("gl" IN_LIST FEATURES)
    if (VCPKG_TARGET_IS_LINUX OR VCPKG_TARGET_IS_BSD)
        list(APPEND required_externals gl)
    endif()
    string(APPEND OPTIONS " skia_use_gl=true")
else()
    string(APPEND OPTIONS " skia_use_gl=false")
endif()

if("metal" IN_LIST FEATURES)
    string(APPEND OPTIONS " skia_use_metal=true")
endif()

if("vulkan" IN_LIST FEATURES)
    list(APPEND required_externals vulkan_headers)
    if (NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        list(APPEND required_externals
            spirv-tools
            spirv-headers
        )
    endif()
    string(APPEND OPTIONS " skia_use_vulkan=true skia_vulkan_memory_allocator_dir=\"${CURRENT_INSTALLED_DIR}\"")
endif()

if("direct3d" IN_LIST FEATURES)
    list(APPEND required_externals
        spirv-cross
        spirv-headers
        d3d12allocator
    )
    string(APPEND OPTIONS " skia_use_direct3d=true")
endif()

if("graphite" IN_LIST FEATURES)
    string(APPEND OPTIONS " skia_enable_graphite=true")
else()
    string(APPEND OPTIONS " skia_enable_graphite=false")
endif()

if("dawn" IN_LIST FEATURES)
    string(APPEND OPTIONS " skia_use_dawn=true")
    declare_external_from_pkgconfig(dawn PATH "third_party/dawn" MODULES unofficial_webgpu_dawn)
    list(APPEND required_externals dawn)
endif()

if("pdf" IN_LIST FEATURES)
    string(APPEND OPTIONS " skia_enable_pdf=true")
else()
    string(APPEND OPTIONS " skia_enable_pdf=false")
endif()

if("jpeg" IN_LIST FEATURES)
    list(APPEND required_externals libjpeg)
    string(APPEND OPTIONS " skia_use_libjpeg_turbo_decode=true skia_use_libjpeg_turbo_encode=true skia_use_no_jpeg_encode=false")
else()
    string(APPEND OPTIONS " skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_no_jpeg_encode=true")
endif()

if("png" IN_LIST FEATURES)
    list(APPEND required_externals libpng)
    string(APPEND OPTIONS " skia_use_libpng_decode=true skia_use_libpng_encode=true skia_use_no_png_encode=false")
else()
    string(APPEND OPTIONS " skia_use_libpng_decode=false skia_use_libpng_encode=false skia_use_no_png_encode=true")
endif()

if("webp" IN_LIST FEATURES)
    list(APPEND required_externals libwebp)
    string(APPEND OPTIONS " skia_use_libwebp_decode=true skia_use_libwebp_encode=true skia_use_no_webp_encode=false")
else()
    string(APPEND OPTIONS " skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_no_webp_encode=true")
endif()

if("avif" IN_LIST FEATURES)
    list(APPEND required_externals libavif)
    string(APPEND OPTIONS " skia_use_libavif=true")
else()
    string(APPEND OPTIONS " skia_use_libavif=false")
endif()

get_externals(${required_externals})
if("icu" IN_LIST FEATURES)
    vcpkg_replace_string("${SOURCE_PATH}/third_party/icu/BUILD.gn"
        [[config("vcpkg_icu") {]]
        [[import("icu.gni")
config("vcpkg_icu")  {]])
endif()

vcpkg_find_acquire_program(PYTHON3)
vcpkg_replace_string("${SOURCE_PATH}/.gn" "script_executable = \"python3\"" "script_executable = \"${PYTHON3}\"")
vcpkg_replace_string("${SOURCE_PATH}/gn/toolchain/BUILD.gn" "python3 " "\\\"${PYTHON3}\\\" ")

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

if(VCPKG_TARGET_IS_WINDOWS)
    string(REGEX REPLACE "[\\]\$" "" WIN_VC "$ENV{VCINSTALLDIR}")
    string(REGEX REPLACE "[\\]\$" "" WIN_SDK "$ENV{WindowsSdkDir}")
    string(APPEND OPTIONS " win_vc=\"${WIN_VC}\"")
    string(APPEND OPTIONS " win_sdk=\"${WIN_SDK}\"")
elseif(VCPKG_TARGET_IS_ANDROID)
    string(APPEND OPTIONS " ndk=\"${VCPKG_DETECTED_CMAKE_ANDROID_NDK}\" ndk_api=${VCPKG_DETECTED_CMAKE_SYSTEM_VERSION}")
else()
    string(APPEND OPTIONS " \
        cc=\"${VCPKG_DETECTED_CMAKE_C_COMPILER}\" \
        cxx=\"${VCPKG_DETECTED_CMAKE_CXX_COMPILER}\"")
endif()

string_to_gn_list(SKIA_C_FLAGS_DBG "${VCPKG_COMBINED_C_FLAGS_DEBUG}")
string_to_gn_list(SKIA_CXX_FLAGS_DBG "${VCPKG_COMBINED_CXX_FLAGS_DEBUG}")
string(APPEND OPTIONS_DBG " \
    extra_cflags_c=${SKIA_C_FLAGS_DBG} \
    extra_cflags_cc=${SKIA_CXX_FLAGS_DBG}")

string_to_gn_list(SKIA_C_FLAGS_REL "${VCPKG_COMBINED_C_FLAGS_RELEASE}")
string_to_gn_list(SKIA_CXX_FLAGS_REL "${VCPKG_COMBINED_CXX_FLAGS_RELEASE}")
string(APPEND OPTIONS_REL " \
    extra_cflags_c=${SKIA_C_FLAGS_REL} \
    extra_cflags_cc=${SKIA_CXX_FLAGS_REL}")

if(VCPKG_TARGET_IS_UWP)
    string_to_gn_list(SKIA_LD_FLAGS "-APPCONTAINER WindowsApp.lib")
    string(APPEND OPTIONS " extra_ldflags=${SKIA_LD_FLAGS}")
endif()

vcpkg_gn_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS "${OPTIONS}"
    OPTIONS_DEBUG "${OPTIONS_DBG}"
    OPTIONS_RELEASE "${OPTIONS_REL}"
)

skia_gn_install(
    SOURCE_PATH "${SOURCE_PATH}"
    TARGETS ${SKIA_TARGETS}
)

# Use skia repository layout in ${CURRENT_PACKAGES_DIR}/include/skia
file(COPY "${SOURCE_PATH}/include"
          "${SOURCE_PATH}/modules"
          "${SOURCE_PATH}/src"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include/skia"
    FILES_MATCHING PATTERN "*.h"
)
auto_clean("${CURRENT_PACKAGES_DIR}/include/skia")

# vcpkg legacy layout omits "include/" component. Just duplicate.
file(COPY "${CURRENT_PACKAGES_DIR}/include/skia/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include/skia")

# vcpkg legacy
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/skiaConfig.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/skia")

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/example/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/example/skcms.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/example/skpathediting.cpp"
    "${SOURCE_PATH}/tools/convert-to-nia.cpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/example"
)
file(APPEND "${CURRENT_PACKAGES_DIR}/share/${PORT}/example/convert-to-nia.cpp" [[
// Test for https://github.com/microsoft/vcpkg/issues/27219
#include "include/core/SkColorSpace.h"
]])

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(GLOB third_party_licenses "${SOURCE_PATH}/third_party_licenses/*")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" ${third_party_licenses})
