ARG OSVER="latest"
FROM fedora:${OSVER} AS upstream

RUN dnf update -y && \
    dnf group install -y 'development-tools' && \
    dnf install -y \
      autoconf-archive \
      automake \
      ccache \
      cmake \
      curl \
      liberation-sans-fonts \
      libglvnd-devel \
      nasm \
      ninja-build \
      perl-FindBin \
      perl-IPC-Cmd \
      perl-lib \
      qt6-qtbase-devel \
      qt6-qtmultimedia-devel \
      qt6-qttools-devel \
      qt6-qtwayland-devel \
      tar \
      unzip \
      zip \
      zlib-ng-compat-static && \
    rm -rf \
      /tmp/* \
      /var/cache \
      /var/log/* \
      /var/tmp/*

RUN groupadd -r -g 9999 ladybird-ci && \
    useradd -m -u 9999 -g 9999 ladybird-ci && \
    mkdir -pv /build && \
    chown ladybird-ci:ladybird-ci /build

USER ladybird-ci
WORKDIR /build

RUN cc --version && c++ --version
