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
      git \
      liberation-sans-fonts \
      libglvnd-devel \
      nasm \
      ninja-build \
      nodejs \
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
      /var/tmp/* && \
    ldconfig && \
    fc-cache -f

RUN groupadd -r -g 9999 ladybird-ci && \
    useradd -m -u 9999 -g 9999 ladybird-ci && \
    mkdir -v -m 1777 /__w && \
    chown -v ladybird-ci:ladybird-ci /__w

USER ladybird-ci
WORKDIR /__w

RUN cc --version && c++ --version
