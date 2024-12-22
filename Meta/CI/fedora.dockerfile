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

RUN groupadd -r -g 1001 ladybird-ci && \
    useradd -m -u 1001 -g 1001 ladybird-ci
USER ladybird-ci
RUN cc --version && c++ --version
