# Ladybird browser build instructions

## Build Prerequisites

Qt6 development packages, nasm, additional build tools, and a C++23 capable compiler are required.

We currently use gcc-14 and clang-20 in our CI pipeline. If these versions are not available on your system, see
[`Meta/find_compiler.py`](../Meta/find_compiler.py) for the minimum compatible version.

CMake 3.25 or newer must be available in $PATH.

> [!NOTE]
> In all of the below lists of packages, the Qt6 multimedia package is not needed if your Linux system supports PulseAudio.

---

### Debian/Ubuntu:

<!-- Note: If you change something here, please also change it in the `devcontainer/devcontainer.json` file. -->
```bash
sudo apt install autoconf autoconf-archive automake build-essential ccache cmake curl fonts-liberation2 git libgl1-mesa-dev nasm ninja-build pkg-config python3-venv qt6-base-dev qt6-tools-dev-tools qt6-wayland tar unzip zip
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

- Recommendation: Install clang from [LLVM's apt repository](https://apt.llvm.org/):

```bash
# Add LLVM GPG signing key
sudo wget -O /usr/share/keyrings/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key

# Optional: Verify the GPG key manually

# Use the key to authorize an entry for apt.llvm.org in apt sources list
echo "deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg.key] https://apt.llvm.org/$(lsb_release -sc)/ llvm-toolchain-$(lsb_release -sc)-20 main" | sudo tee -a /etc/apt/sources.list.d/llvm.list

# Update apt package list and install clang and associated packages
sudo apt update -y && sudo apt install clang-20 clangd-20 clang-tools-20 clang-format-20 clang-tidy-20 lld-20 -y
```

- Alternative: Install gcc from [Ubuntu Toolchain PPA](https://launchpad.net/~ubuntu-toolchain-r/+archive/ubuntu/test):

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update && sudo apt install g++-14 libstdc++-14-dev
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
sudo dnf install autoconf-archive automake ccache cmake curl git liberation-sans-fonts libglvnd-devel nasm ninja-build patchelf perl-FindBin perl-IPC-Cmd perl-lib qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qttools-devel qt6-qtwayland-devel tar unzip zip zlib-ng-compat-static
```

### openSUSE:

```
sudo zypper install autoconf-archive automake ccache cmake curl gcc14 gcc14-c++ git liberation-fonts libglvnd-devel nasm ninja qt6-base-devel qt6-multimedia-devel qt6-tools-devel qt6-wayland-devel tar unzip zip
```

It is currently recommended to install the `libpulse-devel` package to avoid runtime dynamic linking issues

```
sudo zypper install libpulse-devel
```

The build process requires at least python3.7; openSUSE Leap only features Python 3.6 as default, so it is recommendable to install the package `python312` and create a virtual environment (venv) in this case.

A virtual enviroment can be created in your home directory and once the `source` command is issued `python3 --version` will show that the current version is python 3.12 within the virtual environment shell session.
```
python3.12 -m venv ~/python312_venv
source ~/python312_venv/bin/activate
python3 --version
```

This virtual environment can be created once and reused in future shell sessions by sourcing the generated file `~/python312_venv/bin/activate` again.

### Void Linux:

```
sudo xbps-install -Su # (optional) ensure packages are up to date to avoid "Transaction aborted due to unresolved dependencies."
sudo xbps-install -S git bash gcc python3 curl cmake zip unzip linux-headers make pkg-config autoconf automake autoconf-archive nasm MesaLib-devel ninja qt6-base-devel qt6-multimedia-devel qt6-tools-devel qt6-wayland-devel
```

### NixOS or with Nix:

A Nix development shell is maintained [here](https://github.com/nix-community/nix-environments/tree/master/envs/ladybird),
in the [nix-environments](https://github.com/nix-community/nix-environments/) repository. If you encounter any problems
building with Nix, please create an issue there.

### macOS:

Xcode 15 or clang from homebrew is required to successfully build ladybird.

```
xcode-select --install
brew install autoconf autoconf-archive automake ccache cmake nasm ninja pkg-config
```

If you wish to use clang from homebrew instead:
```
brew install llvm@20
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

### Android:

On a Unix-like platform, install the prerequisites for that platform and then see the [Android Studio guide](EditorConfiguration/AndroidStudioConfiguration.md).
Or, download a version of Gradle >= 8.0.0, and run the ``gradlew`` program in ``UI/Android``

### FreeBSD

```
pkg install autoconf-archive automake autoconf bash cmake curl gmake gn libtool libxcb libxkbcommon libX11 libXrender libXi nasm ninja patchelf pkgconf python3 qt6-base qt6-multimedia unzip zip
```

## Build steps

### Using ladybird.py

The simplest way to build and run ladybird is via the ladybird.py script:

```bash
# From /path/to/ladybird
./Meta/ladybird.py run
```

On macOS, to build using clang from homebrew:
```bash
CC=$(brew --prefix llvm)/bin/clang CXX=$(brew --prefix llvm)/bin/clang++ ./Meta/ladybird.py run
```

You may also choose to start it in `gdb` using:
```bash
./Meta/ladybird.py gdb ladybird
```

The above commands will build a Release version of Ladybird. To instead build a Debug version, run the
`Meta/ladybird.py` script with the value of the `BUILD_PRESET` environment variable set to `Debug`, like this:

```bash
BUILD_PRESET=Debug ./Meta/ladybird.py run
```

Note that debug symbols are available in both Release and Debug builds.

If you want to run other applications, such as the the JS REPL or the WebAssembly REPL, specify an executable with
`./Meta/ladybird.py run <executable_name>`.

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

The script Meta/ladybird.py and the default preset in CMakePresets.json both define a build directory of
`Build/release`. For distribution purposes, or when building multiple configurations, it may be useful to create a custom
CMake build directory.

The install rules in UI/cmake/InstallRules.cmake define which binaries and libraries will be
installed into the configured CMAKE_PREFIX_PATH or path passed to ``cmake --install``.

Note that when using a custom build directory rather than Meta/ladybird.py, the user may need to provide a suitable C++
compiler (see [Build Prerequisites](BuildInstructionsLadybird.md#build-prerequisites)) via the CMAKE_C_COMPILER and
CMAKE_CXX_COMPILER cmake options.

```
cmake --preset default -B MyBuildDir
# optionally, add -DCMAKE_CXX_COMPILER=<suitable compiler> -DCMAKE_C_COMPILER=<matching c compiler>
cmake --build --preset default MyBuildDir
ninja -C MyBuildDir run-ladybird
```

### Building with limited system memory

The default build mode will run as many build steps in parallel as possible, which includes link steps;
this may be an issue for users with limited system memory (or users building with fat LTO in general).
If you wish to reduce the number of parallel link jobs, you may use the LAGOM_LINK_POOL_SIZE cmake option
to set a maximum limit for the number of parallel link jobs.

```
cmake --preset default -B MyBuildDir -DLAGOM_LINK_POOL_SIZE=2
```

### Running manually

The Meta/ladybird.py script will execute the `run-ladybird` and `debug-ladybird` custom targets.
If you don't want to use the ladybird.py script to run the application, you can run the following commands:

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

After running Ladybird as suggested above with `./Meta/ladybird.py run ladybird`, you can now in CLion use Run -> Attach to Process to connect. If debugging layout or rendering issues, filter the listing that opens for `WebContent` and attach to that.

Now breakpoints, stepping and variable inspection will work.

### Debugging with Xcode or Instruments on macOS

If all you want to do is use Instruments, then an Xcode project is not required.

Simply run the `ladybird.py` script as normal, and then make sure to codesign the Ladybird binary with the proper entitlements to allow Instruments to attach to it.

```
./Meta/ladybird.py build
 ninja -C Build/release apply-debug-entitlements
 # or
 codesign -s - -v -f --entitlements Meta/debug.plist Build/release/bin/Ladybird.app
```

Now you can open the Instruments app and point it to the Ladybird app bundle.

Building the project with Xcode is not supported. The Xcode project generated by CMake does not properly execute custom
targets, and does not handle all target names in the project.
