# The generated file here is read by vcpkg/base-triplets/base.cmake to ensure consistency between the project
# build and the vcpkg build.
set(EXTRA_VCPKG_VARIABLES "")
if (NOT "${CMAKE_C_COMPILER}" STREQUAL "")
    string(APPEND EXTRA_VCPKG_VARIABLES "set(ENV{CC} ${CMAKE_C_COMPILER})\n")
endif()
if (NOT "${CMAKE_CXX_COMPILER}" STREQUAL "")
    string(APPEND EXTRA_VCPKG_VARIABLES "set(ENV{CXX} ${CMAKE_CXX_COMPILER})\n")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build-vcpkg-variables.cmake" "${EXTRA_VCPKG_VARIABLES}")
