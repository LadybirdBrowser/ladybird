FROM ubuntu:26.04

ARG GCC_VERSION=14
ARG LLVM_VERSION=21
ARG WASM_TOOLS_VERSION=1.243.0

ENV DEBIAN_FRONTEND=noninteractive
ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY rust-toolchain.toml /tmp/rust-toolchain.toml

RUN apt-get update -o APT::Update::Error-Mode=any \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
        lsb-release \
        software-properties-common \
    && UBUNTU_RELEASE="$(lsb_release -cs)" \
    && curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${UBUNTU_RELEASE} main" > /etc/apt/sources.list.d/kitware.list \
    && apt-get update -o APT::Update::Error-Mode=any \
    && apt-get install -y --no-install-recommends \
        at-spi2-core \
        autoconf \
        autoconf-archive \
        automake \
        bison \
        build-essential \
        ccache \
        clang-${LLVM_VERSION} \
        clang-format-${LLVM_VERSION} \
        clang-tools-${LLVM_VERSION} \
        cmake \
        dbus-x11 \
        file \
        fonts-liberation2 \
        g++-${GCC_VERSION} \
        gcc-${GCC_VERSION} \
        generate-ninja \
        gir1.2-atspi-2.0 \
        git \
        jq \
        libcurl4-openssl-dev \
        libdrm-dev \
        libegl1-mesa-dev \
        libgl1-mesa-dev \
        libpulse-dev \
        libssl-dev \
        libstdc++-${GCC_VERSION}-dev \
        libtool \
        libxkbcommon-dev \
        libclang-${LLVM_VERSION}-dev \
        libclang-rt-${LLVM_VERSION}-dev \
        lld-${LLVM_VERSION} \
        llvm-${LLVM_VERSION} \
        llvm-${LLVM_VERSION}-dev \
        nasm \
        ninja-build \
        nodejs \
        npm \
        openssl \
        optipng \
        orca \
        pkg-config \
        pulseaudio \
        python3 \
        python3-dasbus \
        python3-gi \
        python3-psutil \
        python3-venv \
        qt6-base-dev \
        qt6-tools-dev-tools \
        rsync \
        shellcheck \
        tar \
        unzip \
        xvfb \
        xz-utils \
        zip \
        zstd \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-${GCC_VERSION} 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-${GCC_VERSION} 100 \
    && update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/lld lld /usr/bin/lld-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/llvm-symbolizer llvm-symbolizer /usr/bin/llvm-symbolizer-${LLVM_VERSION} 100 \
    && npm install -g prettier@3.8.1 \
    && rustup_init="$(mktemp)" \
    && curl -fsSL https://sh.rustup.rs -o "${rustup_init}" \
    && chmod +x "${rustup_init}" \
    && rust_toolchain="$(grep -E '^channel = ' /tmp/rust-toolchain.toml | cut -d '"' -f 2)" \
    && "${rustup_init}" -y --no-modify-path --profile minimal --default-toolchain "${rust_toolchain}" \
    && rm "${rustup_init}" \
    && rustup component add rustfmt clippy \
    && rm -rf /opt/rustup/downloads /opt/rustup/tmp /opt/cargo/registry /tmp/rust-toolchain.toml \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH

# wasm-tools is required at build time to generate the WebAssembly spec tests
# (INCLUDE_WASM_SPEC_TESTS). The version is pinned for reproducible test generation.
RUN case "${TARGETARCH}" in \
        amd64) WASM_TOOLS_ARCH=x86_64 ;; \
        arm64) WASM_TOOLS_ARCH=aarch64 ;; \
        *) echo "Unsupported TARGETARCH: ${TARGETARCH}" >&2 ; exit 1 ;; \
    esac \
    && NAME="wasm-tools-${WASM_TOOLS_VERSION}-${WASM_TOOLS_ARCH}-linux" \
    && curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors \
        "https://github.com/bytecodealliance/wasm-tools/releases/download/v${WASM_TOOLS_VERSION}/${NAME}.tar.gz" \
        -o /tmp/wasm-tools.tar.gz \
    && tar -xzf /tmp/wasm-tools.tar.gz -C /tmp \
    && install -m 0755 "/tmp/${NAME}/wasm-tools" /usr/local/bin/wasm-tools \
    && rm -rf /tmp/wasm-tools.tar.gz "/tmp/${NAME}" \
    && wasm-tools --version
