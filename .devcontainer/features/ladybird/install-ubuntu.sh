#!/bin/sh
set -e

### Check distro
if [ ! -f /etc/lsb-release ]; then
    echo "SCRIPT ERROR: Not an Ubuntu container"
    exit 1
fi

# shellcheck source=/dev/null
. /etc/lsb-release

### Declare helper functions

install_llvm_key() {
    wget -O /usr/share/keyrings/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key
    echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] http://apt.llvm.org/${DISTRIB_CODENAME}/ llvm-toolchain-${DISTRIB_CODENAME} main" | tee -a /etc/apt/sources.list.d/llvm.list
    if [ ! "${LLVM_VERSION}" = "trunk" ]; then
        echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] http://apt.llvm.org/${DISTRIB_CODENAME}/ llvm-toolchain-${DISTRIB_CODENAME}-${LLVM_VERSION} main" | tee -a /etc/apt/sources.list.d/llvm.list
    fi
    apt update -y
}

### Install packages

apt update -y
apt install -y lsb-release git python3 autoconf autoconf-archive automake build-essential cmake libdrm-dev libgl1-mesa-dev libtool nasm ninja-build pkg-config qt6-base-dev qt6-tools-dev-tools qt6-wayland ccache fonts-liberation2 zip unzip curl tar

### Install Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y

### Ensure new enough host compiler is available

VERSION="0.0.0"
if command -v clang >/dev/null 2>&1; then
    VERSION="$(clang -dumpversion)"
fi
MAJOR_VERSION="${VERSION%%.*}"

if [ "${LLVM_VERSION}" = "trunk" ]; then
    install_llvm_key

    apt install -y llvm clang clangd clang-tools lld lldb clang-tidy clang-format
elif [ "${MAJOR_VERSION}" -lt "${LLVM_VERSION}" ]; then
    FAILED_INSTALL=0
    apt install -y "llvm-${LLVM_VERSION}" "clang-${LLVM_VERSION}" "clangd-${LLVM_VERSION}" "clang-tools-${LLVM_VERSION}" "lld-${LLVM_VERSION}" "lldb-${LLVM_VERSION}" "clang-tidy-${LLVM_VERSION}" "clang-format-${LLVM_VERSION}"  || FAILED_INSTALL=1

    if [ "${FAILED_INSTALL}" -ne 0 ]; then
        install_llvm_key
        apt install -y "llvm-${LLVM_VERSION}" "clang-${LLVM_VERSION}" "clangd-${LLVM_VERSION}" "clang-tools-${LLVM_VERSION}" "lld-${LLVM_VERSION}" "lldb-${LLVM_VERSION}" "clang-tidy-${LLVM_VERSION}" "clang-format-${LLVM_VERSION}"
    fi
fi
