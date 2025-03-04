{
  description = "Ladybird";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default";
  };

  outputs =
    {
      self,
      systems,
      nixpkgs,
      ...
    }:
    let
      eachSystem = nixpkgs.lib.genAttrs (import systems);
    in
    {
      packages = eachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          # Default ladybird package built from source using current directory
          # as the source. This is not cached.
          ladybird = pkgs.callPackage ./Toolchain/Nix/package.nix { inherit self; };

          # Alias for `nix run .` or `nix run github:LadybirdBrowser/ladybird`
          default = self.packages.${system}.ladybird;
        }
      );

      devShells = eachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          # Override nixpkgs ladybird with the one provided
          # by this flake. This will allow inputsFrom to
          # remain in-sync with the package provided by the
          # flake.
          default = pkgs.callPackage ./Toolchain/Nix/shell.nix {
            inherit (self.packages.${system}) ladybird;
          };
        }
      );

      # Provide a formatter for `nix fmt`
      formatter = eachSystem (system: nixpkgs.legacyPackages.${system}.nixfmt-rfc-style);
    };
}
