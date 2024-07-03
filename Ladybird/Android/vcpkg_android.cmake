# This file is based on the vcpkg Android example from here https://github.com/microsoft/vcpkg-docs/blob/06b496c3f24dbe651fb593a26bee50537eeaf4e5/vcpkg/examples/vcpkg_android_example_cmake_script/cmake/vcpkg_android.cmake
# It was modified to use CMake variables instead of environment variables, because it's not possible to set environment variables in the Android Gradle plugin.
#
# vcpkg_android.cmake
#
# Helper script when using vcpkg with cmake. It should be triggered via the variable VCPKG_TARGET_ANDROID
#
# For example:
# if (VCPKG_TARGET_ANDROID)
#     include("cmake/vcpkg_android.cmake")
# endif()
#
# This script will:
# 1 & 2. check the presence of needed env variables: ANDROID_NDK and VCPKG_ROOT
# 3. set VCPKG_TARGET_TRIPLET according to ANDROID_ABI
# 4. Combine vcpkg and Android toolchains by setting CMAKE_TOOLCHAIN_FILE
#    and VCPKG_CHAINLOAD_TOOLCHAIN_FILE

# Note: VCPKG_TARGET_ANDROID is not an official vcpkg variable.
# it is introduced for the need of this script

if (VCPKG_TARGET_ANDROID)

    #
    # 1. Check the presence of variable ANDROID_NDK
    #
    if (NOT DEFINED ANDROID_NDK)
        message(FATAL_ERROR "Please set CMake variable ANDROID_NDK")
    endif()

    #
    # 2. Check the presence of environment variable VCPKG_ROOT
    #
    if (NOT DEFINED VCPKG_ROOT)
        message(FATAL_ERROR "Please set a CMake variable VCPKG_ROOT")
    endif()

    #
    # 3. Set VCPKG_TARGET_TRIPLET according to ANDROID_ABI
    #
    # There are four different Android ABI, each of which maps to
    # a vcpkg triplet. The following table outlines the mapping from vcpkg architectures to android architectures
    #
    # |VCPKG_TARGET_TRIPLET       | ANDROID_ABI          |
    # |---------------------------|----------------------|
    # |arm64-android              | arm64-v8a            |
    # |arm-android                | armeabi-v7a          |
    # |x64-android                | x86_64               |
    # |x86-android                | x86                  |
    #
    # The variable must be stored in the cache in order to successfully the two toolchains.
    #
    if (ANDROID_ABI MATCHES "arm64-v8a")
        set(VCPKG_TARGET_TRIPLET "arm64-android" CACHE STRING "" FORCE)
    elseif(ANDROID_ABI MATCHES "armeabi-v7a")
        set(VCPKG_TARGET_TRIPLET "arm-android" CACHE STRING "" FORCE)
    elseif(ANDROID_ABI MATCHES "x86_64")
        set(VCPKG_TARGET_TRIPLET "x64-android" CACHE STRING "" FORCE)
    elseif(ANDROID_ABI MATCHES "x86")
        set(VCPKG_TARGET_TRIPLET "x86-android" CACHE STRING "" FORCE)
    else()
        message(FATAL_ERROR "
        Please specify ANDROID_ABI
        For example
        cmake ... -DANDROID_ABI=armeabi-v7a

        Possible ABIs are: arm64-v8a, armeabi-v7a, x64-android, x86-android
        ")
    endif()
    message("vcpkg_android.cmake: VCPKG_TARGET_TRIPLET was set to ${VCPKG_TARGET_TRIPLET}")

    #
    # 4. Combine vcpkg and Android toolchains
    #

    # vcpkg and android both provide dedicated toolchains:
    #
    # vcpkg_toolchain_file=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
    # android_toolchain_file=$ANDROID_NDK/build/cmake/android.toolchain.cmake
    #
    # When using vcpkg, the vcpkg toolchain shall be specified first.
    # However, vcpkg provides a way to preload and additional toolchain,
    # with the VCPKG_CHAINLOAD_TOOLCHAIN_FILE option.
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${ANDROID_NDK}/build/cmake/android.toolchain.cmake)
    set(CMAKE_TOOLCHAIN_FILE ${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)
    message("vcpkg_android.cmake: CMAKE_TOOLCHAIN_FILE was set to ${CMAKE_TOOLCHAIN_FILE}")
    message("vcpkg_android.cmake: VCPKG_CHAINLOAD_TOOLCHAIN_FILE was set to ${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")

    # vcpkg depends on the environment variables ANDROID_NDK_HOME and VCPKG_ROOT being set.
    # However, we cannot set those through the Android Gradle plugin (we can only set CMake variables).
    # Therefore, we forward our CMake variables to environment variables.
    # FIXME: would be nice if vcpkg's android toolchain did not require this...
    set(ENV{ANDROID_NDK_HOME} ${ANDROID_NDK})
    set(ENV{VCPKG_ROOT} ${VCPKG_ROOT})

endif(VCPKG_TARGET_ANDROID)
