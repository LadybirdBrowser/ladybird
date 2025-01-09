#!/bin/sh
# Prebuild ladybird's vcpkg dependencies
set -e

# FIXME: Add some options to make this more flexible and usable by other projects
# FIXME: Find a way to do this without cloning ladybird

case "$(uname -m)" in
    x86_64|x64)
      ;;
    *)
      export VCPKG_FORCE_SYSTEM_BINARIES=1
      ;;
esac

cd /tmp

CACHE_DIR=/usr/local/share/vcpkg-binary-cache
mkdir -p ${CACHE_DIR}

# Clone ladybird to get access to vcpkg.json and vcpkg commit id
git clone https://github.com/LadybirdBrowser/ladybird.git --depth 1
cd ladybird
# Grab and bootstrap the exact commit of vcpkg that trunk is using
python3 ./Toolchain/BuildVcpkg.py

# Install the vcpkg.json in manifest mode from the root of the repo
# Set the binary cache directory to the one we intend to use at container runtime
export VCPKG_ROOT="${PWD}/Build/vcpkg"
export VCPKG_BINARY_SOURCES="clear;files,${CACHE_DIR},readwrite"

# Check options to see which versions we should build
if [ "${RELEASE_TRIPLET}" = "true" ]; then
    "${VCPKG_ROOT}/vcpkg" install --overlay-triplets="${PWD}/Meta/CMake/vcpkg/release-triplets"
fi

if [ "${DEBUG_TRIPLET}" = "true" ]; then
    "${VCPKG_ROOT}/vcpkg" install --overlay-triplets="${PWD}/Meta/CMake/vcpkg/debug-triplets"
fi

if [ "${SANITIZER_TRIPLET}" = "true" ]; then
    "${VCPKG_ROOT}/vcpkg" install --overlay-triplets="${PWD}/Meta/CMake/vcpkg/sanitizer-triplets"
fi

# Clean up to reduce layer size
cd /tmp
rm -rf ladybird
