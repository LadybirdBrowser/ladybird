#!/bin/sh
set -e

# Git and GitHub CLI
dnf install -y git gh

# Ladybird dev dependencies
dnf install -y autoconf-archive automake ccache cmake curl gn google-noto-sans-mono-fonts libdrm-devel \
    liberation-sans-fonts libglvnd-devel libtool nasm ninja-build patchelf perl-FindBin perl-IPC-Cmd perl-lib \
    pulseaudio-libs-devel qt6-qtbase-devel qt6-qttools-devel qt6-qtwayland-devel \
    ShellCheck tar unzip zip zlib-ng-compat-static
