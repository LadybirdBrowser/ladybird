#!/bin/sh
# Prebuild ladybird's vcpkg dependencies
set -e

# FIXME: Add some options to make this more flexible and usable by other projects
# FIXME: Find a way to do this without cloning ladybird

case "$(uname -m)" in
    x86_64|x64)
      export VCPKG_DEFAULT_TRIPLET=x64-linux-dynamic
      ;;
    aarch64|arm64)
      export VCPKG_DEFAULT_TRIPLET=arm64-linux-dynamic
      export VCPKG_FORCE_SYSTEM_BINARIES=1
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

# Install the vcpkg.json in manifest mode from the root of the repo
# Set the binary cache directory to the one we intend to use at container runtime
export VCPKG_BINARY_SOURCES="clear;files,${CACHE_DIR},readwrite"
export X_VCPKG_ASSET_SOURCES="clear;x-azurl,https://vcpkg-cache.app.ladybird.org/ladybird/source-assets/,,read"

install_vcpkg()
{
    preset="${1}"
    preset_lower="$(echo "${preset}" | tr '[:upper:]' '[:lower:]')"

    ./Meta/ladybird.py vcpkg --preset "${preset}"
    "./Build/vcpkg-${preset_lower}/vcpkg" install --overlay-triplets="${PWD}/Meta/CMake/vcpkg/${preset_lower}-triplets"
}

# Check options to see which versions we should build
if [ "${RELEASE_TRIPLET}" = "true" ]; then
    install_vcpkg Release
fi
if [ "${DEBUG_TRIPLET}" = "true" ]; then
    install_vcpkg Debug
fi
if [ "${SANITIZER_TRIPLET}" = "true" ]; then
    install_vcpkg Sanitizer
fi

# Clean up to reduce layer size
cd /tmp
rm -rf ladybird
