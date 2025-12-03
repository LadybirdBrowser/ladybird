# Ideally, we would set VCPKG_BUILD_TYPE="debug", but that is currently not supported as a standalone build type.
# See: https://github.com/microsoft/vcpkg/issues/38224
if (NOT DEFINED VCPKG_LIBRARY_LINKAGE)
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
