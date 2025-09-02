#!/usr/bin/env bash

set -e

# Libs
mkdir -p "$FLATPAK_DEST/lib"
for path in out/*.a out/*.so; do
    install -Dm644 "$path" "$FLATPAK_DEST/lib/$(basename "$path")"
done

# Includes
mkdir -p "$FLATPAK_DEST/include/skia/modules"
pushd include
    find . -name '*.h' -exec install -Dm644 {} "$FLATPAK_DEST/include/skia/{}" \;
popd
pushd modules
    find . -name '*.h' -exec install -Dm644 {} "$FLATPAK_DEST/include/skia/modules/{}" \;
popd

# Pkg-config
mkdir -p "$FLATPAK_DEST/lib/pkgconfig"
cat > "$FLATPAK_DEST/lib/pkgconfig/skia.pc" <<EOF
    prefix=${FLATPAK_DEST}
    exec_prefix=\${prefix}
    libdir=\${prefix}/lib
    includedir=\${prefix}/include/skia
    Name: skia
    Description: 2D graphic library for drawing text, geometries and images.
    URL: https://skia.org/
    Version: 144
    Libs: -L\${libdir} -lskia -lskcms
    Cflags: -I\${includedir}
EOF

# Some skia includes are assumed to be under an include sub directory by
# other includes
# shellcheck disable=SC2013
for file in $(grep -rl '#include "include/' "$FLATPAK_DEST/include/skia"); do
  sed -i -e 's|#include "include/|#include "|g' "$file"
done
