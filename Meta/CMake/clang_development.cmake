#
# Finds clang and llvm development packages that match the current clang version
#

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
    return()
endif()

set(DESIRED_CLANG_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

# find zstd package before clang so that it is built fully and not
# specifically for clang's use. Clang/LLVM requires zstd and it's cmake
# will build zstd if not available, but clang's cmake will incorrectly
# build zstd for curl 8.18.0, which also depends on zstd.
#
# see https://github.com/LadybirdBrowser/ladybird/pull/7738
# related to https://github.com/llvm/llvm-project/issues/139666
find_package(zstd QUIET CONFIG REQUIRED)

find_package(Clang "${DESIRED_CLANG_VERSION}" QUIET REQUIRED CONFIG)
find_package(LLVM "${DESIRED_CLANG_VERSION}" QUIET REQUIRED CONFIG)
