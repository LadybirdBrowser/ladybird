{ pkgs ? import <nixpkgs> { } }:

(pkgs.buildFHSEnv {
  name = "nix-shell";
  targetPkgs = pkgs: (with pkgs; [
    autoconf
    autoconf-archive
    automake
    ccache
    cmake
    curl
    ffmpeg.dev
    glibc.dev
    gnutar
    libglvnd.dev
    libxcrypt
    nasm
    ninja
    pkg-config
    python3
    qt6.qtbase
    qt6.qtbase.dev
    qt6.qtmultimedia
    qt6.qttools
    qt6.qtwayland
    qt6.qtwayland.dev
    unzip
    xorg.libX11.dev
    xorg.xorgproto
    zip
  ]);
  profile = ''
    # NOTE: This is required to make it find the wayland platform plugin installed
    #       above, but should probably be fixed upstream.
    export QT_PLUGIN_PATH="$QT_PLUGIN_PATH:${pkgs.qt6.qtwayland}/lib/qt-6/plugins"
    export QT_QPA_PLATFORM="wayland;xcb"
  '';
}).env
