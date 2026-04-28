#!/bin/sh
set -e

# Git and GitHub CLI
dnf install -y git gh

# Ladybird dev dependencies
dnf install -y autoconf-archive automake bison ccache cmake curl google-noto-sans-mono-fonts liberation-sans-fonts \
    libdrm-devel libglvnd-devel libtool nasm ninja-build patchelf perl-FindBin perl-IPC-Cmd perl-lib perl-Time-Piece qt6-qtbase-devel \
    qt6-qttools-devel qt6-qtwayland-devel tar unzip zip zlib-ng-compat-static
