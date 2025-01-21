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

# chromium/5414
set(ANGLE_COMMIT aa63ea230e0c507e7b4b164a30e502fb17168c17)
set(ANGLE_VERSION 5414)
set(ANGLE_SHA512 a3b55d4b484e1e9ece515d60af1d47a80a0576b198d9a2397e4e68b16efd83468dcdfadc98dae57ff17f01d02d74526f8b59fdf00661b70a45b6dd266e5ffe38)
set(ANGLE_THIRDPARTY_ZLIB_COMMIT 44d9b490c721abdb923d5c6c23ac211e45ffb1a5)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/angle
    REF ${ANGLE_COMMIT}
    SHA512 ${ANGLE_SHA512}
    # On update check headers against opengl-registry
    PATCHES
        001-fix-uwp.patch
        002-fix-builder-error.patch
        003-fix-mingw.patch
)

# Generate angle_commit.h
set(ANGLE_COMMIT_HASH_SIZE 12)
string(SUBSTRING "${ANGLE_COMMIT}" 0 ${ANGLE_COMMIT_HASH_SIZE} ANGLE_COMMIT_HASH)
set(ANGLE_COMMIT_DATE "invalid-date")
set(ANGLE_REVISION "${ANGLE_VERSION}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/angle_commit.h.in" "${SOURCE_PATH}/angle_commit.h" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/angle_commit.h.in" "${SOURCE_PATH}/src/common/angle_commit.h" @ONLY)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/unofficial-angle-config.cmake" DESTINATION "${SOURCE_PATH}")

set(ANGLE_WEBKIT_BUILDSYSTEM_COMMIT "bb1da00b9ba878d228a5e9834a0767dbca2fee43")

# Download WebKit gni-to-cmake.py conversion script
vcpkg_download_distfile(GNI_TO_CMAKE_PY
    URLS "https://github.com/WebKit/WebKit/raw/${ANGLE_WEBKIT_BUILDSYSTEM_COMMIT}/Source/ThirdParty/ANGLE/gni-to-cmake.py"
    FILENAME "gni-to-cmake.py"
    SHA512 9da35caf2db2e849d6cc85721ba0b77eee06b6f65a7c5314fb80483db4949b0b6e9bf4b2d4fc63613665629b24e9b052e03fb1451b09313d881297771a4f2736
)

# Generate CMake files from GN / GNI files
vcpkg_find_acquire_program(PYTHON3)

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
    SHA512 63f981694ae37d4c4ca4c34e2bf62b4d4602b6a1a660851304fa7a6ee834fc58fa6730eeb41ef4e075550f3c8b675823d4d00bdcd72ca869c6d5ab11196b33bb
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
    SHA512 0d126b1d1b0ca995c2ea6e51c73326db363f560f3f07912ce58c7c022d9257d27b963dac56aee0e9604ca7a3d74c5aa9f0451c243fec922fb485dd2253685ab6
)
file(COPY "${WK_ANGLE_CMAKE_WEBKITMACROS}" DESTINATION "${SOURCE_PATH}/cmake")

# Copy additional custom CMake buildsystem into appropriate folders
file(GLOB MAIN_BUILDSYSTEM "${CMAKE_CURRENT_LIST_DIR}/cmake-buildsystem/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/cmake-buildsystem/*.cmake")
file(COPY ${MAIN_BUILDSYSTEM} DESTINATION "${SOURCE_PATH}")
file(GLOB MODULES "${CMAKE_CURRENT_LIST_DIR}/cmake-buildsystem/cmake/*.cmake")
file(COPY ${MODULES} DESTINATION "${SOURCE_PATH}/cmake")

function(checkout_in_path PATH URL REF)
    if(EXISTS "${PATH}")
        return()
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
