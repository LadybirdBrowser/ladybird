#!/usr/bin/env bash

# Packages an installed Ladybird build as an AppImage, which names the zsync file that updates it.
#
# Usage: package-appimage.sh BUILD_DIRECTORY VERSION ARCHITECTURE OUTPUT_DIRECTORY
# BUILD_DIRECTORY is a configured and built Distribution build of Linux, and ARCHITECTURE is x86_64 or arm64.

set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 BUILD_DIRECTORY VERSION ARCHITECTURE OUTPUT_DIRECTORY" >&2
    exit 1
fi

version=$2
case "$3" in
    x86_64) architecture=x86_64 ;;
    arm64 | aarch64) architecture=aarch64 ;;
    *)
        echo "Unknown architecture: $3" >&2
        exit 1
        ;;
esac
build_directory=$(realpath "$1")
mkdir -p "$4"
output_directory=$(realpath "$4")

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108

case "$architecture" in
    x86_64)
        LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
        APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
        RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
        ;;
    aarch64)
        LINUXDEPLOY_SHA256=620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
        APPIMAGETOOL_SHA256=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
        RUNTIME_SHA256=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
        ;;
esac

name="ladybird-${version}-Linux-${architecture}.AppImage"
update_information="zsync|https://download.ladybird.org/linux/latest/ladybird-Linux-${architecture}.AppImage.zsync"

work_directory=$(mktemp -d)
trap 'rm -rf "$work_directory"' EXIT

fetch() {
    local url=$1 sha256=$2 file=$3
    curl --fail --location --silent --show-error --output "$work_directory/$file" "$url"
    echo "$sha256  $work_directory/$file" | sha256sum --check --quiet
    chmod +x "$work_directory/$file"
}

fetch "$LINUXDEPLOY_URL/linuxdeploy-$architecture.AppImage" "$LINUXDEPLOY_SHA256" linuxdeploy
fetch "$APPIMAGETOOL_URL/appimagetool-$architecture.AppImage" "$APPIMAGETOOL_SHA256" appimagetool
fetch "$RUNTIME_URL/runtime-$architecture" "$RUNTIME_SHA256" runtime

# The AppImage tools are AppImages themselves, which run without FUSE in a container.
export APPIMAGE_EXTRACT_AND_RUN=1

appdir="$work_directory/AppDir"
for component in ladybird_Runtime ladybird_Freedesktop; do
    cmake --install "$build_directory" --prefix "$appdir/usr" --component "$component" --strip
done
echo AppImage >"$appdir/usr/share/Lagom/package-format"

# linuxdeploy sets the runpath of every library in usr/lib, and for ICU's data library, whose data comes first in the
# file, that moves the data on aarch64 but leaves the symbol that ICU finds it by pointing at the old place. So ICU
# rejects its data, and every use of it fails. That library depends on nothing and needs no runpath, so put back the
# copy that was installed.
mapfile -d '' icu_data_libraries < <(find "$appdir/usr/lib" -maxdepth 1 -type f -name 'libicudata.so.*' -print0)
mkdir "$work_directory/icu-data"
for library in "${icu_data_libraries[@]}"; do
    cp -p "$library" "$work_directory/icu-data/"
done

"$work_directory/linuxdeploy" --appdir "$appdir" \
    --deploy-deps-only "$appdir/usr/lib" --deploy-deps-only "$appdir/usr/libexec" \
    --executable "$appdir/usr/bin/ladybird" \
    --desktop-file "$appdir/usr/share/applications/org.ladybird.Ladybird.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/org.ladybird.Ladybird.svg"

for library in "${icu_data_libraries[@]}"; do
    cp -p "$work_directory/icu-data/$(basename "$library")" "$library"
done

# linuxdeploy gives every library the runpath of one in usr/lib, which the plugins in directories below it do not share.
while IFS= read -r library; do
    if patchelf --print-rpath "$library" >/dev/null 2>&1; then
        patchelf --set-rpath "\$ORIGIN:\$ORIGIN/$(realpath --relative-to="$(dirname "$library")" "$appdir/usr/lib")" "$library"
    fi
done < <(find "$appdir/usr/lib" -mindepth 2 -type f -name '*.so*')

ln -sf usr/bin/ladybird "$appdir/AppRun"

ARCH="$architecture" "$work_directory/appimagetool" --no-appstream --runtime-file "$work_directory/runtime" \
    -u "$update_information" "$appdir" "$output_directory/$name"
