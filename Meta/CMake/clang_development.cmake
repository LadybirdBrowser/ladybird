#
# Finds clang and llvm development packages that match the current clang version
#

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
    return()
endif()

set(DESIRED_CLANG_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

find_package(Clang "${DESIRED_CLANG_VERSION}" QUIET REQUIRED CONFIG)
find_package(LLVM "${DESIRED_CLANG_VERSION}" QUIET REQUIRED CONFIG)
