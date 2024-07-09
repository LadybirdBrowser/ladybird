# Ladybird browser build instructions

## Build Prerequisites

Qt6 development packages and a C++23 capable compiler are required. g++-13 or clang-17 are required at a minimum for c++23 support.

NOTE: In all of the below lists of packages, the Qt6 multimedia package is not needed if your Linux system supports PulseAudio.

---

### On Debian/Ubuntu:

##### For C++23 capable compiler:

- `clang` installation steps:

```bash
# Add LLVM GPG signing key
sudo wget -O /usr/share/keyrings/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key

# Verify the GPG key, and append it to the sources list for apt
echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] https://apt.llvm.org/$(lsb_release -sc)/ llvm-toolchain-$(lsb_release -sc)-18 main" | sudo tee -a /etc/apt/sources.list.d/llvm.list

# Update apt package list and install clang
sudo apt update -y && sudo apt install clang-18 -y
```

##### Required packages include, but are not limited to:

```
sudo apt install autoconf autoconf-archive automake build-essential cmake libavcodec-dev libgl1-mesa-dev nasm ninja-build qt6-base-dev qt6-tools-dev-tools qt6-multimedia-dev ccache fonts-liberation2 zip unzip curl tar libssl-dev
```

For Ubuntu 20.04 and above, ensure that the Qt6 Wayland packages are available:

```
sudo apt install qt6-wayland
```

---

On Arch Linux/Manjaro:

```
sudo pacman -S --needed automake base-devel cmake ffmpeg libgl nasm ninja qt6-base qt6-tools qt6-wayland qt6-multimedia ccache ttf-liberation curl unzip zip tar autoconf-archive
```

On Fedora or derivatives:
```
sudo dnf install automake cmake libglvnd-devel nasm ninja-build qt6-qtbase-devel qt6-qttools-devel qt6-qtwayland-devel qt6-qtmultimedia-devel ccache liberation-sans-fonts curl zip unzip tar autoconf-archive libavcodec-free-devel
```

On openSUSE:
```
sudo zypper install automake cmake libglvnd-devel nasm ninja qt6-base-devel qt6-multimedia-devel qt6-tools-devel qt6-wayland-devel ccache liberation-fonts curl zip unzip tar autoconf-archive ffmpeg-7-libavcodec-devel gcc13 gcc13-c++
```
The build process requires at least python3.7; openSUSE Leap only features Python 3.6 as default, so it is recommendable to install package python311 and create a virtual environment (venv) in this case.

On NixOS or with Nix:
```console
nix develop

# With a custom entrypoint, for example your favorite shell
nix develop --command bash
```

On NixOS or with Nix using your host `nixpkgs` and the legacy `nix-shell` tool:
```console
nix-shell Ladybird

# With a custom entrypoint, for example your favorite shell
nix-shell --command bash Ladybird
```

On macOS:

Xcode 14 versions before 14.3 might crash while building ladybird. Xcode 14.3 or clang from homebrew may be required to successfully build ladybird.

```
xcode-select --install
brew install autoconf autoconf-archive automake cmake ffmpeg nasm ninja ccache pkg-config
```

If you also plan to use the Qt chrome on macOS:
```
brew install qt
```

On OpenIndiana:

Note that OpenIndiana's latest GCC port (GCC 11) is too old to build Ladybird, so you need Clang, which is available in the repository.

```
pfexec pkg install cmake ninja clang-17 libglvnd qt6
```

On Haiku:
```
pkgman install cmake ninja cmd:python3 qt6_base_devel qt6_multimedia_devel qt6_tools_devel openal_devel
```

On Windows:

WSL2/WSLg are preferred, as they provide a linux environment that matches one of the above distributions.
MinGW/MSYS2 are not supported, but may work with sufficient elbow grease. Native Windows builds are not supported with either clang-cl or MSVC.

For Android:

On a Unix-like platform, install the prerequisites for that platform and then see the [Android Studio guide](AndroidStudioConfiguration.md).
Or, download a version of Gradle >= 8.0.0, and run the ``gradlew`` program in ``Ladybird/Android``

## Build steps

### Using ladybird.sh

The simplest way to build and run ladybird is via the ladybird.sh script:

```bash
# From /path/to/ladybird
./Meta/ladybird.sh run ladybird
./Meta/ladybird.sh gdb ladybird
```

The above commands will build a Release version of Ladybird. To instead build a Debug version, run the
`Meta/ladybird.sh` script with the value of the `BUILD_PRESET` environment variable set to `Debug`, like this:

```bash
BUILD_PRESET=Debug ./Meta/ladybird.sh run ladybird
```

Either way, Ladybird will be built with one of the following browser chromes, depending on the platform:
* [Android UI](https://developer.android.com/develop/ui) - The native chrome on Android.
* [AppKit](https://developer.apple.com/documentation/appkit?language=objc) - The native chrome on macOS.
* [Qt](https://doc.qt.io/qt-6/) - The chrome used on all other platforms.

The Qt chrome is available on platforms where it is not the default as well (except on Android). To build the
Qt chrome, install the Qt dependencies for your platform, and enable the Qt chrome via CMake:

```bash
# From /path/to/ladybird
cmake --preset default -DENABLE_QT=ON
```

To re-disable the Qt chrome, run the above command with `-DENABLE_QT=OFF`.

On macOS, to build with clang from homebrew:

```
brew install llvm
CC=$(brew --prefix llvm)/bin/clang CXX=$(brew --prefix llvm)/bin/clang++ ./Meta/ladybird.sh run
```

### Resource files

Ladybird requires resource files from the ladybird/Base/res directory in order to properly load
icons, fonts, and other theming information. These files are copied into the build directory by
special CMake rules. The expected location of resource files can be tweaked by packagers using
the standard CMAKE_INSTALL_DATADIR variable. CMAKE_INSTALL_DATADIR is expected to be a path relative
to CMAKE_INSTALL_PREFIX. If it is not, things will break.

### Custom CMake build directory

The script Meta/ladybird.sh and the default preset in CMakePresets.json both define a build directory of
`Build/ladybird`. For distribution purposes, or when building multiple configurations, it may be useful to create a custom
CMake build directory.

The install rules in Ladybird/cmake/InstallRules.cmake define which binaries and libraries will be
installed into the configured CMAKE_PREFIX_PATH or path passed to ``cmake --install``.

Note that when using a custom build directory rather than Meta/ladybird.sh, the user may need to provide
a suitable C++ compiler (g++ >= 13, clang >= 14, Apple Clang >= 14.3) via the CMAKE_CXX_COMPILER and
CMAKE_C_COMPILER cmake options.

```
cmake -GNinja -B MyBuildDir
# optionally, add -DCMAKE_CXX_COMPILER=<suitable compiler> -DCMAKE_C_COMPILER=<matching c compiler>
cmake --build MyBuildDir
ninja -C MyBuildDir run-ladybird
```

### Running manually

The Meta/ladybird.sh script will execute the `run-ladybird` and `debug-ladybird` custom targets.
If you don't want to use the ladybird.sh script to run the application, you can run the following commands:

To automatically run in gdb:
```
ninja -C Build/ladybird debug-ladybird
```

To run without ninja rule on non-macOS systems:
```
./Build/ladybird/bin/Ladybird
```

To run without ninja rule on macOS:
```
open -W --stdout $(tty) --stderr $(tty) ./Build/ladybird/bin/Ladybird.app

# Or to launch with arguments:
open -W --stdout $(tty) --stderr $(tty) ./Build/ladybird/bin/Ladybird.app --args https://ladybird.dev
```

### Experimental GN build

There is an experimental GN build for Ladybird. It is not officially supported, but it is kept up to date on a best-effort
basis by interested contributors. See the [GN build instructions](../Meta/gn/README.md) for more information.

In general, the GN build organizes ninja rules in a more compact way than the CMake build, and it may be faster on some systems.
GN also allows building host and cross-targets in the same build directory, which is useful for managing dependencies on host tools when
cross-compiling to other platforms.

### Debugging with CLion

Ladybird should be built with debug symbols first. This can be done by adding `-DCMAKE_BUILD_TYPE=Debug` to the cmake command line,
or selecting the Build Type Debug in the CLion CMake profile.

After running Ladybird as suggested above with `./Meta/ladybird.sh run ladybird`, you can now in CLion use Run -> Attach to Process to connect. If debugging layout or rendering issues, filter the listing that opens for `WebContent` and attach to that.

Now breakpoints, stepping and variable inspection will work.

### Debugging with Xcode or Instruments on macOS

If all you want to do is use Instruments, then an Xcode project is not required.

Simply run the `ladybird.sh` script as normal, and then make sure to codesign the Ladybird binary with the proper entitlements to allow Instruments to attach to it.

```
./Meta/ladybird.sh build
 ninja -C build/ladybird apply-debug-entitlements
 # or
 codesign -s - -v -f --entitlements Meta/debug.plist Build/ladybird/bin/Ladybird.app
```

Now you can open the Instruments app and point it to the Ladybird app bundle.

If you want to use Xcode itself for debugging, you will need to generate an Xcode project.
The `ladybird.sh` build script does not know how to generate Xcode projects, so creating the project must be done manually.

```
cmake -GXcode -B Build/ladybird
```

After generating an Xcode project into the specified build directory, you can open `ladybird.xcodeproj` in Xcode. The project has a ton of targets, many of which are generated code.
The only target that needs a scheme is the ladybird app bundle.

### Building on OpenIndiana

OpenIndiana needs some extra environment variables set to make sure it finds all the executables
and directories it needs for the build to work. The cmake files are in a non-standard path that
contains the Qt version (replace 6.2 with the Qt version you have installed) and you need to tell
it to use clang and clang++, or it will use gcc and g++ from GCC 10 which is currently the default
to build packages on OpenIndiana.

When running Ladybird, make sure that XDG_RUNTIME_DIR is set, or it will immediately crash as it
doesn't find a writable directory for its sockets.

```
CMAKE_PREFIX_PATH=/usr/lib/qt/6.2/lib/amd64/cmake cmake -GNinja -B Build/ladybird -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build Build/ladybird
XDG_RUNTIME_DIR=/var/tmp ninja -C Build/ladybird run
```
