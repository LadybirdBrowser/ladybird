# The generated file here is read by vcpkg/base-triplets/base.cmake to ensure consistency between the project
# build and the vcpkg build.
set(EXTRA_VCPKG_VARIABLES "")
if (NOT "${CMAKE_C_COMPILER}" STREQUAL "")
    string(APPEND EXTRA_VCPKG_VARIABLES "set(ENV{CC} ${CMAKE_C_COMPILER})\n")
endif()
if (NOT "${CMAKE_CXX_COMPILER}" STREQUAL "")
    string(APPEND EXTRA_VCPKG_VARIABLES "set(ENV{CXX} ${CMAKE_CXX_COMPILER})\n")
endif()

# Workaround for bad patchelf interaction with binutils 2.43.50
# https://github.com/LadybirdBrowser/ladybird/issues/2149
# https://github.com/microsoft/vcpkg/issues/41576
# https://github.com/NixOS/patchelf/issues/568
# https://bugzilla.redhat.com/show_bug.cgi?id=2319341
if (LINUX AND NOT LAGOM_USE_LINKER)
    string(APPEND EXTRA_VCPKG_VARIABLES "set(ENV{LDFLAGS} -Wl,-z,noseparate-code)\n")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build-vcpkg-variables.cmake" "${EXTRA_VCPKG_VARIABLES}")

# Munge the VCPKG_TRIPLET to correspond to the right one for our presets
# Just make sure not to override if the developer is trying to cross-compile
# or the developer set it manually, or if this is not the first run of CMake
if (NOT DEFINED CACHE{VCPKG_TARGET_TRIPLET} AND NOT DEFINED CACHE{VCPKG_HOST_TRIPLET} AND NOT DEFINED VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
    # Only tweak settings if there's custom triplets defined
    if (NOT DEFINED CACHE{VCPKG_OVERLAY_TRIPLETS})
        return()
    endif()

    # And then, only tweak settings if the triplets are ours
    string(FIND "${VCPKG_OVERLAY_TRIPLETS}" "${CMAKE_CURRENT_SOURCE_DIR}" VCPKG_OVERLAY_TRIPLETS_MATCH)
    if (VCPKG_OVERLAY_TRIPLETS_MATCH EQUAL -1)
        return()
    endif()

    set(arch "")
    set(os "")

    # The CMake way to do uname -{m,s} checks
    cmake_host_system_information(RESULT os_platform QUERY OS_PLATFORM)
    cmake_host_system_information(RESULT os_name QUERY OS_NAME)

    if(os_platform MATCHES "^(x86_64|AMD64|amd64)$")
      set(arch x64)
    elseif(os_platform MATCHES "^(aarch64|arm64|ARM64)$")
        set(arch arm64)
    else()
        message(FATAL_ERROR "Unable to automatically detect architecture for vcpkg, please set VCPKG_TARGET_TRIPLET manually")
    endif()

    if (os_name STREQUAL "Linux")
        set(os linux)
    elseif (os_name MATCHES "Darwin|macOS")
        set(os osx)
    elseif (os_name MATCHES "Windows")
        set (os windows)
    else()
        message(FATAL_ERROR "Unable to automatically detect os name for vcpkg, please set VCPKG_TARGET_TRIPLET manually")
    endif()

    set(full_triplet "${arch}-${os}")

    # NOTE: This will break if we start putting a trailing / on the triplet paths :|
    cmake_path(GET VCPKG_OVERLAY_TRIPLETS FILENAME triplet_path)
    string(REPLACE "-triplets" "" triplet_path ${triplet_path})
    string(TOLOWER ${triplet_path} triplet_path)
    if (NOT triplet_path STREQUAL "distribution")
        if (NOT os_name MATCHES "Windows") #NOTE: Windows defaults to dynamic linking
            set(full_triplet "${full_triplet}-dynamic")
        endif()
    elseif (os_name MATCHES "Windows")
        set(full_triplet "${full_triplet}-static")
    endif()

    message(STATUS "Determined host VCPKG_TARGET_TRIPLET: ${full_triplet}")
    set(VCPKG_TARGET_TRIPLET ${full_triplet} CACHE STRING "")
    set(VCPKG_HOST_TRIPLET ${full_triplet} CACHE STRING "")
endif()
