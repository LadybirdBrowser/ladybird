FROM gcr.io/oss-fuzz-base/base-builder:ubuntu-24-04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update -o APT::Update::Error-Mode=any \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
        lsb-release \
    && curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" > /etc/apt/sources.list.d/kitware.list \
    && apt-get update -o APT::Update::Error-Mode=any \
    && apt-get install -y --no-install-recommends \
        autoconf \
        autoconf-archive \
        automake \
        build-essential \
        cmake \
        fonts-liberation2 \
        git \
        glslang-tools \
        libdrm-dev \
        libgl1-mesa-dev \
        libncurses-dev \
        libtool \
        nasm \
        ninja-build \
        pkg-config \
        python3-venv \
        tar \
        unzip \
        zip \
    && ln -sf /usr/bin/cmake /usr/local/bin/cmake \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV RUSTUP_INIT_SKIP_PATH_CHECK=yes
ENV PATH=/opt/cargo/bin:$PATH

COPY rust-toolchain.toml vcpkg.json $SRC/ladybird/
WORKDIR $SRC/ladybird

RUN rustup_init="$(mktemp)" \
    && curl -fsSL https://sh.rustup.rs -o "$rustup_init" \
    && chmod +x "$rustup_init" \
    && rust_toolchain="$(sed -n 's/^channel = "\([^"]*\)"/\1/p' rust-toolchain.toml)" \
    && "$rustup_init" -y --no-modify-path --profile minimal --default-toolchain "$rust_toolchain" \
    && rustup component add --toolchain "$rust_toolchain" rustfmt clippy \
    && rm "$rustup_init"

# Cache the compiler-independent dependency bootstrap in the builder image.
RUN vcpkg_revision="$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])')" \
    && git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git Build/vcpkg-distribution \
    && git -C Build/vcpkg-distribution -c advice.detachedHead=false checkout "$vcpkg_revision" \
    && Build/vcpkg-distribution/bootstrap-vcpkg.sh -disableMetrics

COPY . $SRC/ladybird

COPY .clusterfuzzlite/build.sh $SRC/
