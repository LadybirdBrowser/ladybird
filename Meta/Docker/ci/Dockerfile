FROM ubuntu:26.04

ARG GCC_VERSION=16
ARG LLVM_VERSION=22
ARG RUSTUP_VERSION=1.29.3
ARG RUSTUP_INIT_SHA256_X86_64=b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55
ARG RUSTUP_INIT_SHA256_AARCH64=9f608c142e0e38a78957d0661ba1dfaf0b6fb37184cfaa4b9a0aa57af6184282
ARG WASM_TOOLS_VERSION=1.243.0
ARG WASM_TOOLS_SHA256_X86_64=f261622f8015d38ebe9c3345cc2f7bb5de055d3a66ab44efdf78f11068ed9d9f
ARG WASM_TOOLS_SHA256_AARCH64=ad06ba3c527992a1e6e9a7e807cc2bb914072f0a0ae6ce71680de91b1054d2e9

ENV DEBIAN_FRONTEND=noninteractive
ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

COPY rust-toolchain.toml /tmp/ladybird/rust-toolchain.toml
COPY install-build-dependencies.sh /tmp/ladybird/Meta/Docker/ci/install-build-dependencies.sh

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
    && /tmp/ladybird/Meta/Docker/ci/install-build-dependencies.sh \
    && apt-get install -y --no-install-recommends \
        at-spi2-core \
        ccache \
        clang-format-${LLVM_VERSION} \
        clang-tools-${LLVM_VERSION} \
        dbus-x11 \
        g++-${GCC_VERSION} \
        gcc-${GCC_VERSION} \
        generate-ninja \
        gir1.2-atspi-2.0 \
        jq \
        libcurl4-openssl-dev \
        libssl-dev \
        libclang-${LLVM_VERSION}-dev \
        llvm-${LLVM_VERSION}-dev \
        nodejs \
        npm \
        openssl \
        optipng \
        orca \
        python3-dasbus \
        python3-gi \
        python3-psutil \
        rsync \
        sccache \
        shellcheck \
        xvfb \
        zstd \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-${LLVM_VERSION} 100 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-${GCC_VERSION} 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-${GCC_VERSION} 100 \
    && npm install -g prettier@3.8.1 \
    && rm -rf /opt/rustup/downloads /opt/rustup/tmp /opt/cargo/registry /tmp/ladybird /var/lib/apt/lists/*
