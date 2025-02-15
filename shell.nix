{
  pkgs ? import <nixpkgs> { },
}:

(pkgs.buildFHSEnv {
  name = "nix-shell";

  targetPkgs = pkgs:
    with pkgs;
    [
      cmake
      ninja
      pkg-config
      python3

      curl
      ffmpeg
      fontconfig
      libavif
      libGL
      libjxl
      libwebp
      libxcrypt
      openssl
      qt6Packages.qtbase
      qt6Packages.qtmultimedia
      simdutf
      (skia.overrideAttrs (prev: {
        gnFlags = prev.gnFlags ++ [
          # https://github.com/LadybirdBrowser/ladybird/commit/af3d46dc06829dad65309306be5ea6fbc6a587ec
          # https://github.com/LadybirdBrowser/ladybird/commit/4d7b7178f9d50fff97101ea18277ebc9b60e2c7c
          # Remove when/if this gets upstreamed in skia.
          "extra_cflags+=[\"-DSKCMS_API=__attribute__((visibility(\\\"default\\\")))\"]"
        ];
      }))
      woff2

      qt6Packages.qtbase.dev
      qt6Packages.qttools
      qt6Packages.qtwayland.dev

      ccache
      clang-tools
      nodePackages.prettier
      pre-commit

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

      # Linux-specific
      libpulseaudio.dev
      qt6Packages.qtwayland

      # Darwin-specific (optionals fails for reasons?)
      #apple-sdk_14
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
