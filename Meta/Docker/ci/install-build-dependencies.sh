#!/usr/bin/env bash

# Installs what building Ladybird needs, for the CI image and for the release builds in bare distribution containers.
# The CI image's Dockerfile pins the versions and digests of what this installs, which its build passes in and which are
# read from it otherwise.

set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CI_DOCKERFILE="${SOURCE_DIR}/Meta/Docker/ci/Dockerfile"

ci_image_argument() {
    local value="${!1:-}"
    if [ -n "${value}" ]; then
        echo "${value}"
        return
    fi
    value="$(sed -n "s/^ARG $1=//p" "${CI_DOCKERFILE}")"
    if [ -z "${value}" ]; then
        echo "${CI_DOCKERFILE} has no ARG $1." >&2
        exit 1
    fi
    echo "${value}"
}

download() {
    local url="$1" sha256="$2" output="$3"
    curl --fail --silent --show-error --location --retry 5 --retry-all-errors --output "${output}" "${url}"
    echo "${sha256}  ${output}" | sha256sum --check --quiet
}

ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64) ARCH_ARGUMENT=X86_64 ;;
    aarch64) ARCH_ARGUMENT=AARCH64 ;;
    *) echo "Installing the build dependencies on ${ARCH} is not supported." >&2; exit 1 ;;
esac

LLVM_VERSION="$(ci_image_argument LLVM_VERSION)"
GCC_VERSION="$(ci_image_argument GCC_VERSION)"
RUSTUP_VERSION="$(ci_image_argument RUSTUP_VERSION)"
RUSTUP_INIT_SHA256="$(ci_image_argument "RUSTUP_INIT_SHA256_${ARCH_ARGUMENT}")"
WASM_TOOLS_VERSION="$(ci_image_argument WASM_TOOLS_VERSION)"
WASM_TOOLS_SHA256="$(ci_image_argument "WASM_TOOLS_SHA256_${ARCH_ARGUMENT}")"

# shellcheck source=/dev/null
. /etc/os-release

case "${ID}" in
    ubuntu)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -o APT::Update::Error-Mode=any
        apt-get install -y --no-install-recommends \
            autoconf \
            autoconf-archive \
            automake \
            build-essential \
            ca-certificates \
            clang-"${LLVM_VERSION}" \
            cmake \
            curl \
            dpkg-dev \
            file \
            fonts-liberation2 \
            git \
            libclang-rt-"${LLVM_VERSION}"-dev \
            libdrm-dev \
            libegl1-mesa-dev \
            libgl1-mesa-dev \
            libncurses-dev \
            libpulse-dev \
            libstdc++-"${GCC_VERSION}"-dev \
            libtool \
            libxcb-icccm4-dev \
            lld-"${LLVM_VERSION}" \
            llvm-"${LLVM_VERSION}" \
            nasm \
            ninja-build \
            pkg-config \
            pulseaudio \
            python3 \
            python3-venv \
            qt6-base-private-dev \
            qt6-positioning-dev \
            qt6-tools-dev-tools \
            tar \
            unzip \
            xz-utils \
            zip
        for tool in clang clang++ ld.lld lld llvm-symbolizer; do
            update-alternatives --install "/usr/bin/${tool}" "${tool}" "/usr/bin/${tool}-${LLVM_VERSION}" 100
        done
        ;;
    fedora)
        dnf install -y --setopt=install_weak_deps=False \
            autoconf \
            autoconf-archive \
            automake \
            "clang(major) = ${LLVM_VERSION}" \
            cmake \
            compiler-rt \
            curl \
            file \
            findutils \
            gcc-c++ \
            git \
            glslang \
            libdrm-devel \
            liberation-sans-fonts \
            libglvnd-devel \
            libstdc++-devel \
            libtool \
            lld \
            llvm \
            nasm \
            ncurses-devel \
            ninja-build \
            patchelf \
            perl-FindBin \
            perl-IPC-Cmd \
            perl-lib \
            perl-Time-Piece \
            pulseaudio \
            pulseaudio-libs-devel \
            python3 \
            qt6-qtbase-private-devel \
            qt6-qtpositioning-devel \
            qt6-qttools-devel \
            qt6-qtwayland-devel \
            rpm-build \
            tar \
            unzip \
            which \
            xcb-util-wm-devel \
            xz \
            zip \
            zlib-ng-compat-static
        ;;
    *)
        echo "Installing the build dependencies on ${PRETTY_NAME:-${ID}} is not supported." >&2
        exit 1
        ;;
esac

clang_major="$(clang -dumpversion | cut -d . -f 1)"
if [ "${clang_major}" != "${LLVM_VERSION}" ]; then
    echo "${PRETTY_NAME} installed Clang ${clang_major}, but Ladybird builds with Clang ${LLVM_VERSION}." >&2
    exit 1
fi

export RUSTUP_HOME=/opt/rustup
export CARGO_HOME=/opt/cargo
rust_toolchain="$(sed -n 's/^channel = "\(.*\)"/\1/p' "${SOURCE_DIR}/rust-toolchain.toml")"
# Install the components that rust-toolchain.toml lists along with the toolchain, because rustup can fail to add them
# later, when cargo first runs, in GitHub's job containers ("detected conflict").
rust_components="$(sed -n 's/^components = \[\(.*\)\]/\1/p' "${SOURCE_DIR}/rust-toolchain.toml" | tr -d '" ')"
rustup_init_directory="$(mktemp -d)"
download "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/${ARCH}-unknown-linux-gnu/rustup-init" \
    "${RUSTUP_INIT_SHA256}" "${rustup_init_directory}/rustup-init"
chmod +x "${rustup_init_directory}/rustup-init"
"${rustup_init_directory}/rustup-init" -y --no-modify-path --profile minimal --default-toolchain "${rust_toolchain}" \
    --component "${rust_components}"
rm "${rustup_init_directory}/rustup-init"
rmdir "${rustup_init_directory}"
rm -rf "${RUSTUP_HOME}/downloads" "${RUSTUP_HOME}/tmp" "${CARGO_HOME}/registry"
if [ -n "${GITHUB_ENV:-}" ]; then
    {
        echo "RUSTUP_HOME=${RUSTUP_HOME}"
        echo "CARGO_HOME=${CARGO_HOME}"
    } >> "${GITHUB_ENV}"
    echo "${CARGO_HOME}/bin" >> "${GITHUB_PATH}"
fi

# wasm-tools is required at build time to generate the WebAssembly spec tests (INCLUDE_WASM_SPEC_TESTS). The version is
# pinned for reproducible test generation.
wasm_tools="wasm-tools-${WASM_TOOLS_VERSION}-${ARCH}-linux"
wasm_tools_archive="$(mktemp)"
download "https://github.com/bytecodealliance/wasm-tools/releases/download/v${WASM_TOOLS_VERSION}/${wasm_tools}.tar.gz" \
    "${WASM_TOOLS_SHA256}" "${wasm_tools_archive}"
tar -xzf "${wasm_tools_archive}" -C /usr/local/bin --strip-components=1 "${wasm_tools}/wasm-tools"
rm "${wasm_tools_archive}"
wasm-tools --version
