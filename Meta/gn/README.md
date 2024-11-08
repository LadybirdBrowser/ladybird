# gn build for Ladybird

Warning! The GN build is experimental and best-effort. It might not work, and if you use it you're expected to feel comfortable to unbreak it if necessary. Ladybird's official build system is CMake, if in doubt use that. If you add files, you're expected to update the CMake build but you don't need to update GN build files. Reviewers should not ask authors to update GN build files. Keeping the GN build files up-to-date is on the people who use the GN build.

GN is a metabuild system. It always creates ninja files, but it can create some IDE projects (MSVC, Xcode, ...) which then shell out to ninja for the actual build.

This is a good [overview of GN](https://docs.google.com/presentation/d/15Zwb53JcncHfEwHpnG_PoIbbzQ3GQi_cpujYwbpcbZo/edit#slide=id.g119d702868_0_12).

For more information, motivation, philosophy, and inspiration, see the LLVM documentation on its [GN build](https://github.com/llvm/llvm-project/tree/main/llvm/utils/gn#quick-start)

# Creating a gn build

To create a GN build, you need to have GN installed. It's available in some Linux package managers, but if not, or on other OSes, see below.
On Ubuntu 22.04, the main package repos do not have an up to date enough package for GN, so you will need to build it from source or get a binary from Google.

The easiest way to build GN from source is to use our [Toolchain/BuildGN.sh](../../Toolchain/BuildGN.sh) script, which will
drop the built binary into the `Toolchain/Local/gn/bin` directory. The instructions for downloading a prebuilt binary from Google are
[here](https://gn.googlesource.com/gn/+/refs/heads/main#getting-a-binary).

Once you have GN installed, you can create a build directory by running the following commands:

```sh
gn gen out
```

`gn gen` creates a ninja build in the `out` directory. You can then build the project with ninja:

```sh
ninja -C out
```

If GN or ninja report a bunch of errors, it's likely that you need to create an `args.gn` that points to all the required tools.
`args.gn` belongs at the root of the build directory, and can be placed there before running gn gen, or modified with
`gn args <build dir>`. See the section below for a typical `args.gn`.

If you modify `args.gn` outside of `gn args`, be sure to run `gn gen` again to regenerate the ninja files.


# Typical gn args

On macOS, the default args should work out of the box. For compiling Ladybird there won't be any tailoring needed if you have Qt6 installed via homebrew and the Xcode tools installed.

On Ubuntu, it's likely that the default ``cc`` and ``c++`` will not be able to compile the project. For compiling Ladybird, a typical ``args.gn`` might look like the below:

args.gn
```gn
# Set build arguments here. See `gn help buildargs`.
# Chosen clang must be >= version 15.0.0
host_cc="clang"
host_cxx="clang++"
is_clang=true
use_lld=true
qt_install_headers="/usr/include/x86_64-linux-gnu/qt6/"
qt_install_lib="/usr/lib/x86_64-linux-gnu"
qt_install_libexec="/usr/lib/qt6/libexec/"
```

As with any gn project, ``gn args <build dir> --list`` is your best friend.

# Running binaries from the GN build

Targets in the gn build are prefixed by the directory they are declared in. For example, to build the default target
in the Ladybird/ directory and LibWeb, you would run:

```shell
ninja -C out Ladybird
ninja -C out Userland/Libraries/LibWeb
```

Binaries are placed in the `out/bin` directory, and can be run from there.

```shell
./out/bin/Ladybird
# or on macOS
open -W --stdout $(tty) --stderr $(tty) ./out/bin/Ladybird.app --args https://ladybird.dev
```
