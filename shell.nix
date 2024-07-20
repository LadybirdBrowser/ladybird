{
  pkgs ? import <nixpkgs> { },
}:

(pkgs.buildFHSEnv {
  name = "nix-shell";

  inputsFrom = [
    pkgs.ladybird
  ];

  targetPkgs = pkgs:
    with pkgs;
    with pkgs.qt6Packages;
    with pkgs.nodePackages;
    [
      qtbase.dev
      qttools
      qtwayland.dev

      ccache
      clang-tools
      pre-commit
      prettier

      autoconf
      autoconf-archive
      automake
      curl
      ffmpeg.dev
      glibc.dev
      gnutar
      libglvnd.dev
      nasm
      unzip
      xorg.libX11.dev
      xorg.xorgproto
      zip
    ];

  # Fix for: https://github.com/LadybirdBrowser/ladybird/issues/371#issuecomment-2616415434
  # https://github.com/NixOS/nixpkgs/commit/049a854b4be087eaa3a09012b9c452fbc838dd41
  NIX_LDFLAGS = "-lGL";

  shellHook = ''
    # NOTE: This is required to make it find the wayland platform plugin installed
    #       above, but should probably be fixed upstream.
    export QT_PLUGIN_PATH="$QT_PLUGIN_PATH:${pkgs.qt6.qtwayland}/lib/qt-6/plugins"
    export QT_QPA_PLATFORM="wayland;xcb"
  '';
}).env
