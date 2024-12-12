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
