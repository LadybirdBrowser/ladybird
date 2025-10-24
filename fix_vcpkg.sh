#!/bin/bash
set -e

echo "Cleaning up corrupted vcpkg..."
rm -rf /mnt/c/Development/Projects/ladybird/ladybird/Build/vcpkg
rm -rf /mnt/c/Development/Projects/ladybird/ladybird/Build/debug

echo "Creating vcpkg in WSL home directory..."
mkdir -p ~/vcpkg-cache
cd ~/vcpkg-cache

if [ ! -d "vcpkg" ]; then
    echo "Cloning vcpkg to WSL filesystem..."
    git clone https://github.com/microsoft/vcpkg.git
    cd vcpkg
    ./bootstrap-vcpkg.sh
else
    echo "vcpkg already exists in WSL filesystem"
    cd vcpkg
fi

echo "Creating symlink from Windows project to WSL vcpkg..."
ln -sf ~/vcpkg-cache/vcpkg /mnt/c/Development/Projects/ladybird/ladybird/Build/vcpkg

echo ""
echo "vcpkg fixed! Now run:"
echo "  cd /mnt/c/Development/Projects/ladybird/ladybird"
echo "  bash build_ladybird.sh"
