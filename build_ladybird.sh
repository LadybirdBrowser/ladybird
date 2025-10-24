#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "Setting environment variables..."
export VCPKG_ROOT=$HOME/vcpkg-cache/vcpkg
export CC=clang-20
export CXX=clang++-20

echo "Configuring Ladybird with CMake..."
cmake --preset Debug

echo ""
echo "========================================="
echo "Configuration complete!"
echo "========================================="
echo ""
echo "To build Ladybird, run:"
echo "  cmake --build Build/debug"
echo ""
