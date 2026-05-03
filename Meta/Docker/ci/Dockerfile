FROM ubuntu:24.04

ARG GCC_VERSION=14
ARG LLVM_VERSION=21

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
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm-snapshot.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] http://apt.llvm.org/${UBUNTU_RELEASE}/ llvm-toolchain-${UBUNTU_RELEASE}-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
    && curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${UBUNTU_RELEASE} main" > /etc/apt/sources.list.d/kitware.list \
    && apt-get update -o APT::Update::Error-Mode=any \
    && apt-get install -y --no-install-recommends \
        autoconf \
        autoconf-archive \
        automake \
        bison \
        build-essential \
        ccache \
        clang-${LLVM_VERSION} \
        clang++-${LLVM_VERSION} \
        clang-format-${LLVM_VERSION} \
        clang-tools-${LLVM_VERSION} \
        cmake \
        file \
        fonts-liberation2 \
        g++-${GCC_VERSION} \
        gcc-${GCC_VERSION} \
        generate-ninja \
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
        lld-${LLVM_VERSION} \
        llvm-${LLVM_VERSION} \
        llvm-${LLVM_VERSION}-dev \
        nasm \
        ninja-build \
        nodejs \
        npm \
        openssl \
        optipng \
        pkg-config \
        pulseaudio \
        python3 \
        python3-venv \
        qt6-base-dev \
        qt6-tools-dev-tools \
        shellcheck \
        tar \
        unzip \
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
