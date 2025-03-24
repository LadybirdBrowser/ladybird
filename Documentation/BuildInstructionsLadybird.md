# Ladybird browser build instructions

## Build Prerequisites

Qt6 development packages, nasm, additional build tools, and a C++23 capable compiler like g++-13 or clang-17 are required.

CMake 3.25 or newer must be available in $PATH.

> [!NOTE]
> In all of the below lists of packages, the Qt6 multimedia package is not needed if your Linux system supports PulseAudio.

---

### Debian/Ubuntu:

<!-- Note: If you change something here, please also change it in the `devcontainer/devcontainer.json` file. -->
```bash
sudo apt install autoconf autoconf-archive automake build-essential ccache cmake curl fonts-liberation2 git libgl1-mesa-dev nasm ninja-build pkg-config qt6-base-dev qt6-tools-dev-tools qt6-wayland tar unzip zip
```

#### CMake 3.25 or newer:

- Recommendation: Install `CMake 3.25` or newer from [Kitware's apt repository](https://apt.kitware.com/):

> [!NOTE]
> This repository is Ubuntu-only

```bash
# Add Kitware GPG signing key
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null

# Optional: Verify the GPG key manually

# Use the key to authorize an entry for apt.kitware.com in apt sources list
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -sc) main" | sudo tee /etc/apt/sources.list.d/kitware.list

# Update apt package list and install cmake
sudo apt update -y && sudo apt install cmake -y
```

#### C++23-capable compiler:

- Recommendation: Install `clang-17` or newer from [LLVM's apt repository](https://apt.llvm.org/):

```bash
# Add LLVM GPG signing key
sudo wget -O /usr/share/keyrings/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key

# Optional: Verify the GPG key manually

# Use the key to authorize an entry for apt.llvm.org in apt sources list
echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] https://apt.llvm.org/$(lsb_release -sc)/ llvm-toolchain-$(lsb_release -sc)-19 main" | sudo tee -a /etc/apt/sources.list.d/llvm.list

# Update apt package list and install clang and associated packages
sudo apt update -y && sudo apt install clang-19 clangd-19 clang-format-19 clang-tidy-19 lld-19 -y
```

- Alternative: Install gcc-13 or newer from [Ubuntu Toolchain PPA](https://launchpad.net/~ubuntu-toolchain-r/+archive/ubuntu/test):

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update && sudo apt install g++-13 libstdc++-13-dev
```

#### Audio support:

- Recommendation: Install PulseAudio development package:

```bash
sudo apt install libpulse-dev
```

- Alternative: Install Qt6's multimedia package:

```bash
sudo apt install qt6-multimedia-dev
```

### Arch Linux/Manjaro:

```
sudo pacman -S --needed autoconf-archive automake base-devel ccache cmake curl libgl nasm ninja qt6-base qt6-multimedia qt6-tools qt6-wayland ttf-liberation tar unzip zip
```

### Fedora or derivatives:
```
sudo dnf install autoconf-archive automake ccache cmake curl liberation-sans-fonts libglvnd-devel nasm ninja-build perl-FindBin perl-IPC-Cmd perl-lib qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qttools-devel qt6-qtwayland-devel tar unzip zip zlib-ng-compat-static
```

### openSUSE:
```
sudo zypper install autoconf-archive automake ccache cmake curl gcc13 gcc13-c++ liberation-fonts libglvnd-devel nasm ninja qt6-base-devel qt6-multimedia-devel qt6-tools-devel qt6-wayland-devel tar unzip zip
```
The build process requires at least python3.7; openSUSE Leap only features Python 3.6 as default, so it is recommendable to install package python311 and create a virtual environment (venv) in this case.

### Void Linux:
```
sudo xbps-install -Su # (optional) ensure packages are up to date to avoid "Transaction aborted due to unresolved dependencies."
sudo xbps-install -S git bash gcc python3 curl cmake zip unzip linux-headers make pkg-config autoconf automake autoconf-archive nasm MesaLib-devel ninja qt6-base-devel qt6-multimedia-devel qt6-tools-devel qt6-wayland-devel
```

### macOS:

Xcode 15 or clang from homebrew is required to successfully build ladybird.

```
xcode-select --install
brew install autoconf autoconf-archive automake ccache cmake nasm ninja pkg-config
```

If you wish to use clang from homebrew instead:
```
brew install llvm@19
```

If you also plan to use the Qt UI on macOS:
```
brew install qt
```

> [!NOTE]
> It is recommended to add your terminal application (i.e. Terminal.app or iTerm.app) to the system list of developer tools.
> Doing so will reduce slow startup time of freshly compiled binaries, due to macOS validating the binary on its first run.
> This can be done in the "Developer Tools" section of the "Privacy & Security" system settings.

### Windows:

WSL2 is the supported way to build Ladybird on Windows. An experimental native build is being setup but does not fully
build.

#### WSL2
- Create a WSL2 environment using one of the Linux distros listed above. Ubuntu or Fedora is recommended.

- Install the required packages for the selected Linux distro in the WSL2 environment.

WSL1 is known to have issues. If you run into problems, please use WSL2.

MinGW/MSYS2 are not supported.

##### Clang-CL (experimental)

> [!NOTE]
> This only gets the cmake to configure. There is still a lot of work to do in terms of getting it to build.

In order to get pkg-config available for the vcpkg install, you can use Chocolatey to install it.
To install Chocolatey, see `https://chocolatey.org/install`.

Then Install pkg-config using chocolatey.
```
choco install pkgconfiglite -y
```

### OpenIndiana:

Note that OpenIndiana's latest GCC port (GCC 11) is too old to build Ladybird, so you need Clang, which is available in the repository.

```
pfexec pkg install clang-17 cmake libglvnd ninja qt6
```

### Haiku:
```
pkgman install cmake cmd:python3 ninja openal_devel qt6_base_devel qt6_multimedia_devel qt6_tools_devel
```

### Android:

> [!NOTE]
> The Android port is currently [broken](https://github.com/LadybirdBrowser/ladybird/issues/2774) and won't build.

On a Unix-like platform, install the prerequisites for that platform and then see the [Android Studio guide](EditorConfiguration/AndroidStudioConfiguration.md).
Or, download a version of Gradle >= 8.0.0, and run the ``gradlew`` program in ``UI/Android``

## Build steps

### Using ladybird.sh

The simplest way to build and run ladybird is via the ladybird.sh script:

```bash
# From /path/to/ladybird
./Meta/ladybird.sh run
```

On macOS, to build using clang from homebrew:
```bash
CC=$(brew --prefix llvm)/bin/clang CXX=$(brew --prefix llvm)/bin/clang++ ./Meta/ladybird.sh run
```

You may also choose to start it in `gdb` using:
```bash
./Meta/ladybird.sh gdb ladybird
```

The above commands will build a Release version of Ladybird. To instead build a Debug version, run the
`Meta/ladybird.sh` script with the value of the `BUILD_PRESET` environment variable set to `Debug`, like this:

```bash
BUILD_PRESET=Debug ./Meta/ladybird.sh run
```

Note that debug symbols are available in both Release and Debug builds.

If you want to run other applications, such as the headless-browser, the JS REPL, or the WebAssembly REPL, specify an
executable with `./Meta/ladybird.sh run <executable_name>`.

### The User Interfaces

Ladybird will be built with one of the following browser frontends, depending on the platform:
* [AppKit](https://developer.apple.com/documentation/appkit?language=objc) - The native UI on macOS.
* [Qt](https://doc.qt.io/qt-6/) - The UI used on all other platforms.
* [Android UI](https://developer.android.com/develop/ui) - The native UI on Android.

The Qt UI is available on platforms where it is not the default as well (except on Android). To build the
Qt UI, install the Qt dependencies for your platform, and enable the Qt UI via CMake:

```bash
# From /path/to/ladybird
cmake --preset default -DENABLE_QT=ON
```

To re-disable the Qt UI, run the above command with `-DENABLE_QT=OFF`.

### Build error messages you may encounter

The section lists out some particular error messages you may run into, and explains how to deal with them.

#### Unable to find a build program corresponding to "Ninja"

This error message is a red herring. We use vcpkg to manage our third-party dependencies, and this error is logged when
something went wrong building those dependencies. The output in your terminal will vary depending on what exactly went
wrong, but it should look something like:

```
error: building skia:x64-linux failed with: BUILD_FAILED
Elapsed time to handle skia:x64-linux: 1.6 s

-- Running vcpkg install - failed
CMake Error at Toolchain/Tarballs/vcpkg/scripts/buildsystems/vcpkg.cmake:899 (message):
  vcpkg install failed.  See logs for more information:
  Build/release/vcpkg-manifest-install.log
Call Stack (most recent call first):
  /usr/share/cmake-3.30/Modules/CMakeDetermineSystem.cmake:146 (include)
  CMakeLists.txt:15 (project)

CMake Error: CMake was unable to find a build program corresponding to "Ninja".  CMAKE_MAKE_PROGRAM is not set.  You probably need to select a different build tool.
-- Configuring incomplete, errors occurred!  See logs for more information:
  Build/release/vcpkg-manifest-install.log
```

If the error is not immediately clear from the terminal output, be sure to check the specified `vcpkg-manifest-install.log`.
for more information.

### Resource files

Ladybird requires resource files from the ladybird/Base/res directory in order to properly load
icons, fonts, and other theming information. These files are copied into the build directory by
special CMake rules. The expected location of resource files can be tweaked by packagers using
the standard CMAKE_INSTALL_DATADIR variable. CMAKE_INSTALL_DATADIR is expected to be a path relative
to CMAKE_INSTALL_PREFIX. If it is not, things will break.

### Custom CMake build directory

The script Meta/ladybird.sh and the default preset in CMakePresets.json both define a build directory of
`Build/release`. For distribution purposes, or when building multiple configurations, it may be useful to create a custom
CMake build directory.

The install rules in UI/cmake/InstallRules.cmake define which binaries and libraries will be
installed into the configured CMAKE_PREFIX_PATH or path passed to ``cmake --install``.

Note that when using a custom build directory rather than Meta/ladybird.sh, the user may need to provide
a suitable C++ compiler (g++ >= 13, clang >= 14, Apple Clang >= 14.3) via the CMAKE_CXX_COMPILER and
CMAKE_C_COMPILER cmake options.

```
cmake --preset default -B MyBuildDir
# optionally, add -DCMAKE_CXX_COMPILER=<suitable compiler> -DCMAKE_C_COMPILER=<matching c compiler>
cmake --build --preset default MyBuildDir
ninja -C MyBuildDir run-ladybird
```

### Running manually

The Meta/ladybird.sh script will execute the `run-ladybird` and `debug-ladybird` custom targets.
If you don't want to use the ladybird.sh script to run the application, you can run the following commands:

To automatically run in gdb:
```
ninja -C Build/release debug-ladybird
```

To run without ninja rule on non-macOS systems:
```
./Build/release/bin/Ladybird
```

To run without ninja rule on macOS:
```
open -W --stdout $(tty) --stderr $(tty) ./Build/release/bin/Ladybird.app

# Or to launch with arguments:
open -W --stdout $(tty) --stderr $(tty) ./Build/release/bin/Ladybird.app --args https://ladybird.dev
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
 ninja -C Build/release apply-debug-entitlements
 # or
 codesign -s - -v -f --entitlements Meta/debug.plist Build/release/bin/Ladybird.app
```

Now you can open the Instruments app and point it to the Ladybird app bundle.

Building the project with Xcode is not supported. The Xcode project generated by CMake does not properly execute custom
targets, and does not handle all target names in the project.

### Building on OpenIndiana

OpenIndiana needs some extra environment variables set to make sure it finds all the executables
and directories it needs for the build to work. The cmake files are in a non-standard path that
contains the Qt version (replace 6.2 with the Qt version you have installed) and you need to tell
it to use clang and clang++, or it will use gcc and g++ from GCC 10 which is currently the default
to build packages on OpenIndiana.

When running Ladybird, make sure that XDG_RUNTIME_DIR is set, or it will immediately crash as it
doesn't find a writable directory for its sockets.

```
CMAKE_PREFIX_PATH=/usr/lib/qt/6.2/lib/amd64/cmake cmake -GNinja -B Build/release -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build Build/release
XDG_RUNTIME_DIR=/var/tmp ninja -C Build/release run
```
