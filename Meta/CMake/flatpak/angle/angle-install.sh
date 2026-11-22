#!/usr/bin/env bash

set -e

cd angle

# Headers
pushd include
    find . -type f -and -name "*.h" -exec install -D -m644 {} "$FLATPAK_DEST/include/angle/"{} \;
popd

# Libraries
libs=(
  libEGL.so
  libGLESv1_CM.so
  libGLESv2.so
  libchrome_zlib.so
  libfeature_support.so
  libthird_party_abseil-cpp_absl.so
)

for lib in "${libs[@]}"; do
    install -D -m644 out/"$lib" "$FLATPAK_DEST/lib/$lib"
done

# Licenses: flatpak-builder only records those at the top of a module's source, and ANGLE is checked out below it,
# with the third-party code that it builds in.
licenses="$FLATPAK_DEST/share/licenses/$FLATPAK_ID/angle"
install -D -m644 LICENSE "$licenses/LICENSE"
shopt -s nullglob
# Some keep their license texts in a LICENSES directory.
for license in {,src/,src/common/}third_party/*/{LICENSE,COPYING}* third_party/*/{src,source}/{LICENSE,COPYING}*; do
    for file in "$license" "$license"/*; do
        [[ -f "$file" ]] && install -D -m644 "$file" "$licenses/${file//\//_}"
    done
done
shopt -u nullglob

# Pkg-config
mkdir -p "$FLATPAK_DEST/lib/pkgconfig"
cat > "$FLATPAK_DEST/lib/pkgconfig/angle.pc" <<EOF
    prefix=${FLATPAK_DEST}
    exec_prefix=\${prefix}
    libdir=\${prefix}/lib
    includedir=\${prefix}/include
    Name: angle
    Description: A conformant OpenGL ES implementation for Windows, Mac, Linux, iOS and Android.
    URL: https://angleproject.org/
    Version: 7258
    Libs: -L\${libdir} -lEGL -lGLESv2
    Cflags: -I\${includedir}/angle
EOF
