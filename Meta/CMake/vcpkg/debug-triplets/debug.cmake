# Ideally, we would set VCPKG_BUILD_TYPE="debug", but that is currently not supported as a standalone build type.
# See: https://github.com/microsoft/vcpkg/issues/38224
set(VCPKG_LIBRARY_LINKAGE dynamic)
