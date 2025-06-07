if (VCPKG_TARGET_IS_LINUX)
    message(WARNING "Building with a gcc version less than 6.1 is not supported.")
    message(WARNING "${PORT} currently requires the following libraries from the system package manager:\n    libx11-dev\n    mesa-common-dev\n    libxi-dev\n    libxext-dev\n\nThese can be installed on Ubuntu systems via apt-get install libx11-dev mesa-common-dev libxi-dev libxext-dev.")
endif()

if (VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    set(ANGLE_CPU_BITNESS ANGLE_IS_32_BIT_CPU)
elseif (VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(ANGLE_CPU_BITNESS ANGLE_IS_64_BIT_CPU)
elseif (VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
    set(ANGLE_CPU_BITNESS ANGLE_IS_32_BIT_CPU)
elseif (VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(ANGLE_CPU_BITNESS ANGLE_IS_64_BIT_CPU)
else()
    message(FATAL_ERROR "Unsupported architecture: ${VCPKG_TARGET_ARCHITECTURE}")
endif()

set(ANGLE_USE_D3D11_COMPOSITOR_NATIVE_WINDOW "OFF")
if (VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_UWP)
  set(ANGLE_BUILDSYSTEM_PORT "Win")
  if (NOT VCPKG_TARGET_IS_MINGW)
    set(ANGLE_USE_D3D11_COMPOSITOR_NATIVE_WINDOW "ON")
  endif()
elseif (VCPKG_TARGET_IS_OSX)
  set(ANGLE_BUILDSYSTEM_PORT "Mac")
elseif (VCPKG_TARGET_IS_LINUX)
  set(ANGLE_BUILDSYSTEM_PORT "Linux")
else()
  # default other platforms to "Linux" config
  set(ANGLE_BUILDSYSTEM_PORT "Linux")
endif()

set(USE_METAL OFF)
if ("metal" IN_LIST FEATURES)
    set(USE_METAL ON)
endif()

# chromium/7067
set(ANGLE_COMMIT 48103cb2f2b292cb50cc5a29546b358b2e47fd29)
set(ANGLE_VERSION 7085)
set(ANGLE_SHA512 d3ff7fdef0989bfebb660a2935fa5ec7788e67d59ebecca475ba3c9b1e6ea84e77e8d0130ff6d8457ae4f24885d572b8ac0d44d599ed25fbd38c0f4749c6fe82)
set(ANGLE_THIRDPARTY_ZLIB_COMMIT 788cb3c270e8700b425c7bdca1f9ce6b0c1400a9)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/angle
    REF ${ANGLE_COMMIT}
    SHA512 ${ANGLE_SHA512}
    # On update check headers against opengl-registry
    PATCHES
        001-fix-builder-error.patch
)

# Generate angle_commit.h
set(ANGLE_COMMIT_HASH_SIZE 12)
string(SUBSTRING "${ANGLE_COMMIT}" 0 ${ANGLE_COMMIT_HASH_SIZE} ANGLE_COMMIT_HASH)
set(ANGLE_COMMIT_DATE "invalid-date")
set(ANGLE_REVISION "${ANGLE_VERSION}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/angle_commit.h.in" "${SOURCE_PATH}/angle_commit.h" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/angle_commit.h.in" "${SOURCE_PATH}/src/common/angle_commit.h" @ONLY)

# Generate ANGLEShaderProgramVersion.h.in
# FIXME: ANGLE's build system hashes the renderer files to determine the program version hash.
#        For now, we'll just use the ANGLE commit hash.
#        See: https://github.com/google/angle/commit/82826be01fcc4d02a637312f4df3ba97e74f7226#diff-81195814d06b98e6258a63901769078f42c522448b2847a33bd51e24ac9faef6
set(ANGLE_PROGRAM_VERSION_HASH_SIZE 12)
string(SUBSTRING "${ANGLE_COMMIT}" 0 ${ANGLE_PROGRAM_VERSION_HASH_SIZE} ANGLE_PROGRAM_VERSION)
configure_file("${CMAKE_CURRENT_LIST_DIR}/ANGLEShaderProgramVersion.h.in" "${SOURCE_PATH}/ANGLEShaderProgramVersion.h" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/ANGLEShaderProgramVersion.h.in" "${SOURCE_PATH}/src/common/ANGLEShaderProgramVersion.h" @ONLY)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/unofficial-angle-config.cmake" DESTINATION "${SOURCE_PATH}")

set(ANGLE_WEBKIT_BUILDSYSTEM_COMMIT "624839dfc8c8b65c9ac45252fb1121ded39e3366")

# Download WebKit gni-to-cmake.py conversion script
vcpkg_download_distfile(GNI_TO_CMAKE_PY
    URLS "https://github.com/WebKit/WebKit/raw/${ANGLE_WEBKIT_BUILDSYSTEM_COMMIT}/Source/ThirdParty/ANGLE/gni-to-cmake.py"
    FILENAME "gni-to-cmake.py"
    SHA512 51ca45d4d2384d641b6672cb7cdfac200c58889b4b4cb83f1b04c1a0a2c9ab8b68f1c90d77763983684bcde674b073cfd85cfc160285332c0414d8ec6397601b
)

# Generate CMake files from GN / GNI files
x_vcpkg_get_python_packages(PYTHON_VERSION "3" OUT_PYTHON_VAR "PYTHON3" PACKAGES ply)

set(_root_gni_files_to_convert
  "compiler.gni Compiler.cmake"
  "libGLESv2.gni GLESv2.cmake"
)
set(_renderer_gn_files_to_convert
  "libANGLE/renderer/d3d/BUILD.gn D3D.cmake"
  "libANGLE/renderer/gl/BUILD.gn GL.cmake"
  "libANGLE/renderer/metal/BUILD.gn Metal.cmake"
)

foreach(_root_gni_file IN LISTS _root_gni_files_to_convert)
  separate_arguments(_file_values UNIX_COMMAND "${_root_gni_file}")
  list(GET _file_values 0 _src_gn_file)
  list(GET _file_values 1 _dst_file)
  vcpkg_execute_required_process(
      COMMAND "${PYTHON3}" "${GNI_TO_CMAKE_PY}" "src/${_src_gn_file}" "${_dst_file}"
      WORKING_DIRECTORY "${SOURCE_PATH}"
      LOGNAME "gni-to-cmake-${_dst_file}-${TARGET_TRIPLET}"
  )
endforeach()

foreach(_renderer_gn_file IN LISTS _renderer_gn_files_to_convert)
  separate_arguments(_file_values UNIX_COMMAND "${_renderer_gn_file}")
  list(GET _file_values 0 _src_gn_file)
  list(GET _file_values 1 _dst_file)
  get_filename_component(_src_dir "${_src_gn_file}" DIRECTORY)
  vcpkg_execute_required_process(
      COMMAND "${PYTHON3}" "${GNI_TO_CMAKE_PY}" "src/${_src_gn_file}" "${_dst_file}" --prepend "src/${_src_dir}/"
      WORKING_DIRECTORY "${SOURCE_PATH}"
      LOGNAME "gni-to-cmake-${_dst_file}-${TARGET_TRIPLET}"
  )
endforeach()

# Fetch additional CMake files from WebKit ANGLE buildsystem
vcpkg_download_distfile(WK_ANGLE_INCLUDE_CMAKELISTS
    URLS "https://github.com/WebKit/WebKit/raw/${ANGLE_WEBKIT_BUILDSYSTEM_COMMIT}/Source/ThirdParty/ANGLE/include/CMakeLists.txt"
    FILENAME "include_CMakeLists.txt"
    SHA512 a7ddf3c6df7565e232f87ec651cc4fd84240b8866609e23e3e6e41d22532fd34c70e0f3b06120fd3d6d930ca29c1d0d470d4c8cb7003a66f8c1a840a42f32949
)
configure_file("${WK_ANGLE_INCLUDE_CMAKELISTS}" "${SOURCE_PATH}/include/CMakeLists.txt" COPYONLY)

vcpkg_download_distfile(WK_ANGLE_CMAKE_WEBKITCOMPILERFLAGS
    URLS "https://github.com/WebKit/WebKit/raw/${ANGLE_WEBKIT_BUILDSYSTEM_COMMIT}/Source/cmake/WebKitCompilerFlags.cmake"
    FILENAME "WebKitCompilerFlags.cmake"
    SHA512 006173ad9a4138bf546023d6a46b05b834bbd4e94a443655ebf08b90ca722f4cb4a200a33fde8483f69d0b1f883948035db5c8b784c4f21729d1298222f98eba
)
file(COPY "${WK_ANGLE_CMAKE_WEBKITCOMPILERFLAGS}" DESTINATION "${SOURCE_PATH}/cmake")

vcpkg_download_distfile(WK_ANGLE_CMAKE_DETECTSSE2
    URLS "https://github.com/WebKit/WebKit/raw/${ANGLE_WEBKIT_BUILDSYSTEM_COMMIT}/Source/cmake/DetectSSE2.cmake"
    FILENAME "DetectSSE2.cmake"
    SHA512 219a4c8591ee31d11eb3d1e4803cc3c9d4573984bb25ecac6f2c76e6a3dab598c00b0157d0f94b18016de6786e49d8b29a161693a5ce23d761c8fe6a798c1bca
)
file(COPY "${WK_ANGLE_CMAKE_DETECTSSE2}" DESTINATION "${SOURCE_PATH}/cmake")

vcpkg_download_distfile(WK_ANGLE_CMAKE_WEBKITMACROS
    URLS "https://github.com/WebKit/WebKit/raw/${ANGLE_WEBKIT_BUILDSYSTEM_COMMIT}/Source/cmake/WebKitMacros.cmake"
    FILENAME "WebKitMacros.cmake"
    SHA512 565175443d5d1b8119af504164bf93840e8c786fc479e45feb98ca542351b91d2ea00265d7f8dfa6960975de81802bc43b2a2af2c90fc44bda1ae46d96c89247
)
file(COPY "${WK_ANGLE_CMAKE_WEBKITMACROS}" DESTINATION "${SOURCE_PATH}/cmake")

# Copy additional custom CMake buildsystem into appropriate folders
file(GLOB MAIN_BUILDSYSTEM "${CMAKE_CURRENT_LIST_DIR}/cmake-buildsystem/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/cmake-buildsystem/*.cmake")
file(COPY ${MAIN_BUILDSYSTEM} DESTINATION "${SOURCE_PATH}")
file(GLOB MODULES "${CMAKE_CURRENT_LIST_DIR}/cmake-buildsystem/cmake/*.cmake")
file(COPY ${MODULES} DESTINATION "${SOURCE_PATH}/cmake")

function(checkout_in_path PATH URL REF)
    vcpkg_from_git(
        OUT_SOURCE_PATH DEP_SOURCE_PATH
        URL "${URL}"
        REF "${REF}"
    )
    file(RENAME "${DEP_SOURCE_PATH}" "${PATH}")
    file(REMOVE_RECURSE "${DEP_SOURCE_PATH}")
endfunction()

checkout_in_path(
    "${SOURCE_PATH}/third_party/zlib"
    "https://chromium.googlesource.com/chromium/src/third_party/zlib"
    "${ANGLE_THIRDPARTY_ZLIB_COMMIT}"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS_DEBUG -DDISABLE_INSTALL_HEADERS=1
    OPTIONS
        "-D${ANGLE_CPU_BITNESS}=1"
        "-DPORT=${ANGLE_BUILDSYSTEM_PORT}"
        "-DANGLE_USE_D3D11_COMPOSITOR_NATIVE_WINDOW=${ANGLE_USE_D3D11_COMPOSITOR_NATIVE_WINDOW}"
        "-DVCPKG_TARGET_IS_WINDOWS=${VCPKG_TARGET_IS_WINDOWS}"
        "-DUSE_METAL=${USE_METAL}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH share/unofficial-angle PACKAGE_NAME unofficial-angle)

vcpkg_copy_pdbs()

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
# Remove empty directories inside include directory
file(GLOB directory_children RELATIVE "${CURRENT_PACKAGES_DIR}/include" "${CURRENT_PACKAGES_DIR}/include/*")
foreach(directory_child ${directory_children})
    if(IS_DIRECTORY "${CURRENT_PACKAGES_DIR}/include/${directory_child}")
        file(GLOB_RECURSE subdirectory_children "${CURRENT_PACKAGES_DIR}/include/${directory_child}/*")
        if("${subdirectory_children}" STREQUAL "")
            file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/${directory_child}")
        endif()
    endif()
endforeach()
unset(subdirectory_children)
unset(directory_child)
unset(directory_children)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
