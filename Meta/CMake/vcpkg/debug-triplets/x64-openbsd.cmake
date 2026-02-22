include (${CMAKE_CURRENT_LIST_DIR}/../base-triplets/x64-openbsd.cmake)
include (${CMAKE_CURRENT_LIST_DIR}/debug.cmake)

# OpenBSD's dynamic linker support for RUNPATH and RPATH is too limited for our vcpkg setup
set(VCPKG_BUILD_TYPE static)
