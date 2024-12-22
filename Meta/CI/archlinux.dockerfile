ARG OSVER="base-devel"
FROM ghcr.io/archlinux/archlinux:${OSVER} AS upstream

RUN pacman -Syu --noconfirm && \
    pacman -Sy --noconfirm --needed \
      autoconf-archive \
      automake \
      base-devel \
      ccache \
      cmake \
      curl \
      libgl \
      nasm \
      ninja \
      qt6-base \
      qt6-multimedia \
      qt6-tools \
      qt6-wayland \
      ttf-liberation \
      tar \
      unzip \
      zip && \
    pacman -Scc --noconfirm && \
    ldconfig && \
    fc-cache -f

RUN groupadd -r -g 9999 ladybird-ci && \
    useradd -m -u 9999 -g 9999 ladybird-ci && \
    mkdir -pv /build && \
    chown ladybird-ci:ladybird-ci /build

USER ladybird-ci
WORKDIR /build

RUN cc --version && c++ --version
