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
      git \
      libgl \
      nasm \
      ninja \
      nodejs \
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

RUN groupadd -r -g 1001 ladybird-ci && \
    useradd -m -u 1001 -g 1001 ladybird-ci
USER ladybird-ci
RUN cc --version && c++ --version
