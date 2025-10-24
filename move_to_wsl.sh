#!/bin/bash
set -e

echo "Copying Ladybird source to WSL filesystem..."
echo "This will take a few minutes..."

mkdir -p ~/ladybird-build
cd ~/ladybird-build

if [ ! -d "ladybird" ]; then
    echo "Copying from /mnt/c/Development/Projects/ladybird/ladybird..."
    cp -r /mnt/c/Development/Projects/ladybird/ladybird .
else
    echo "Ladybird already exists in ~/ladybird-build"
fi

cd ladybird

echo "Setting up vcpkg..."
export VCPKG_ROOT=$HOME/vcpkg-cache/vcpkg
export CC=clang-20
export CXX=clang++-20

echo "Configuring build..."
cmake --preset Debug

echo ""
echo "========================================="
echo "Build configured successfully!"
echo "========================================="
echo ""
echo "To build, run:"
echo "  cd ~/ladybird-build/ladybird"
echo "  cmake --build Build/debug -j$(nproc)"
