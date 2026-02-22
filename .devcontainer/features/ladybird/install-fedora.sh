#!/bin/sh
set -e

# Git and GitHub CLI
dnf install -y git gh

# Ladybird dev dependencies
dnf install -y autoconf-archive automake ccache cmake curl google-noto-sans-mono-fonts liberation-sans-fonts \
    libglvnd-devel libtool nasm ninja-build patchelf perl-FindBin perl-IPC-Cmd perl-lib qt6-qtbase-devel \
    qt6-qttools-devel qt6-qtwayland-devel tar unzip zip zlib-ng-compat-static

# Install Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
