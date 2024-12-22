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

USER root
RUN mkdir -pv /build
WORKDIR /build
RUN cc --version && c++ --version
