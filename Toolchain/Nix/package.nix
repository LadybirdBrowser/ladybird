# Override ladybird from nixpkgs with the source directory as the source
# and latest revision (or "dirty") as the version. Unless new dependencies
# are introduced, this should work just fine. If a new dependency is added
# and builds are broken, simply adding the missing dependencies in buildInputs
# inside of the attribute should do the trick.
{
  self,
  lib,
  ladybird,
  ...
}:
ladybird.overrideAttrs {
  # Reproducible source path
  src = builtins.path {
    path = ../../.;
    name = "ladybird-source";
    filter = lib.cleanSourceFilter;
  };

  # Short rev will be missing in "dirty" source trees. In which case we fall
  # back to "dirty" as the version.
  version = self.shortRev or "dirty";

  # Let's not bother nixpkgs maintainers with issues in the source.
  meta.maintainers = with lib.maintainers; [ NotAShelf ];
}
