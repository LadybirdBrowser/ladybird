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
  libEGL_vulkan_secondaries.so
  libGLESv1_CM.so
  libGLESv2.so
  libGLESv2_vulkan_secondaries.so
  libGLESv2_with_capture.so
  libchrome_zlib.so
  libfeature_support.so
  libthird_party_abseil-cpp_absl.so
)

for lib in "${libs[@]}"; do
    install -D -m644 out/"$lib" "$FLATPAK_DEST/lib/$lib"
done

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
