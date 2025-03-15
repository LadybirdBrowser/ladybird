{
  mkShell,
  kdePackages,
  ccache,
  clang-tools,
  pre-commit,
  nodePackages,
  ladybird,
  ...
}:
mkShell {
  inputsFrom = [
    ladybird
  ];

  packages = [
    ccache
    clang-tools
    pre-commit
    nodePackages.prettier
  ];

  # Fix for: https://github.com/LadybirdBrowser/ladybird/issues/371#issuecomment-2616415434
  # https://github.com/NixOS/nixpkgs/commit/049a854b4be087eaa3a09012b9c452fbc838dd41
  NIX_LDFLAGS = "-lGL";

  shellHook = ''
    # NOTE: This is required to make it find the wayland platform plugin installed
    #       above, but should probably be fixed upstream.
    export QT_PLUGIN_PATH="$QT_PLUGIN_PATH:${kdePackages.qtwayland}/lib/qt-6/plugins"
    export QT_QPA_PLATFORM="wayland;xcb"
  '';
}
