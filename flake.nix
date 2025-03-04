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
      devShells = eachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.callPackage ./Nix/shell.nix {
            inherit (self.packages.${system}) ladybird;
          };
        }
      );

      # Provide a formatter for `nix fmt`
      formatter = eachSystem (system: nixpkgs.legacyPackages.${system}.nixfmt-rfc-style);
    };
}
