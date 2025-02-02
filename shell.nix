{
  pkgs ? import <nixpkgs> { },
}:

pkgs.mkShell {
  inputsFrom = [
    pkgs.ladybird
  ];

  packages =
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
    ];

  shellHook = ''
    # NOTE: This is required to make it find the wayland platform plugin installed
    #       above, but should probably be fixed upstream.
    export QT_PLUGIN_PATH="$QT_PLUGIN_PATH:${pkgs.qt6.qtwayland}/lib/qt-6/plugins"
    export QT_QPA_PLATFORM="wayland;xcb"
  '';
}
