set(VCPKG_CMAKE_SYSTEM_NAME OpenBSD)
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)

# Dynamic libraries cause issues on OpenBSD
set(VCPKG_LIBRARY_LINKAGE static)

include(${CMAKE_CURRENT_LIST_DIR}/base.cmake)
