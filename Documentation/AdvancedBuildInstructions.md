# Advanced Build Instructions

This file covers a few advanced scenarios that go beyond what the basic build guide provides.

## Ninja build targets

The `Meta/ladybird.sh` script provides an abstraction over the build targets which are made available by CMake. The
following build targets cannot be accessed through the script and have to be used directly by changing the current
directory to `Build/x86_64` and then running `ninja <target>`:

- `ninja check-style`: Runs the same linters the CI does to verify project style on changed files
- `ninja lint-shell-scripts`: Checks style of shell scripts in the source tree with shellcheck
- `ninja all_generated`: Builds all generated code. Useful for running analysis tools that can use compile_commands.json without a full system build
- `ninja configure-components`: See the [Component Configuration](#component-configuration) section below.

## CMake build options

There are some optional features that can be enabled during compilation that are intended to help with specific types of development work or introduce experimental features. Currently, the following build options are available:
- `ENABLE_ADDRESS_SANITIZER`: builds in runtime checks for memory corruption bugs (like buffer overflows and memory leaks) in Lagom test cases.
- `ENABLE_MEMORY_SANITIZER`: enables runtime checks for uninitialized memory accesses in Lagom test cases.
- `ENABLE_UNDEFINED_SANITIZER`: builds in runtime checks for [undefined behavior](https://en.wikipedia.org/wiki/Undefined_behavior) (like null pointer dereferences and signed integer overflows) in Lagom and the SerenityOS userland.
- `UNDEFINED_BEHAVIOR_IS_FATAL`: makes all undefined behavior sanitizer errors non-recoverable. This option reduces the performance overhead of `ENABLE_UNDEFINED_SANITIZER`.
- `ENABLE_COMPILER_EXPLORER_BUILD`: Skip building non-library entities in Lagom (this only applies to Lagom).
- `ENABLE_FUZZERS`: builds [fuzzers](../Meta/Lagom/ReadMe.md#fuzzing) for various parts of the system.
- `ENABLE_FUZZERS_LIBFUZZER`: builds Clang libFuzzer-based [fuzzers](../Meta/Lagom/ReadMe.md#fuzzing) for various parts of the system.
- `ENABLE_FUZZERS_OSSFUZZ`: builds OSS-Fuzz compatible [fuzzers](../Meta/Lagom/ReadMe.md#fuzzing) for various parts of the system.
- `ENABLE_ALL_THE_DEBUG_MACROS`: used for checking whether debug code compiles on CI. This should not be set normally, as it clutters the console output and makes the system run very slowly. Instead, enable only the needed debug macros, as described below.
- `ENABLE_COMPILETIME_FORMAT_CHECK`: checks for the validity of `std::format`-style format string during compilation. Enabled by default.
- `BUILD_LAGOM`: builds [Lagom](../Meta/Lagom/ReadMe.md), which makes various SerenityOS libraries and programs available on the host system.
- `ENABLE_MOLD_LINKER`: builds the userland with the [`mold` linker](https://github.com/rui314/mold). `mold` can be built by running `Toolchain/BuildMold.sh`.
- `ENABLE_JAKT`: builds the `jakt` compiler as a Lagom host tool and enables building applications and libraries that are written in the jakt language.
- `JAKT_SOURCE_DIR`: `jakt` developer's local checkout of the jakt programming language for rapid testing. To use a local checkout, set to an absolute path when changing the CMake cache of Lagom. e.g. ``cmake -S Meta/Lagom -B Build/lagom -DENABLE_JAKT=ON -DJAKT_SOURCE_DIR=/home/me/jakt``
- `INCLUDE_WASM_SPEC_TESTS`: downloads and includes the WebAssembly spec testsuite tests. In order to use this option, you will need to install `prettier` and `wabt`. wabt version 1.0.23 or higher is required to pre-process the WebAssembly spec testsuite.
- `INCLUDE_FLAC_SPEC_TESTS`: downloads and includes the xiph.org FLAC test suite.
- `BUILD_<component>`: builds the specified component, e.g. `BUILD_HEARTS` (note: must be all caps). Check the components.ini file in your build directory for a list of available components. Make sure to run `ninja clean` and `rm -rf Build/x86_64/Root` after disabling components. These options can be easily configured by using the `ConfigureComponents` utility. See the [Component Configuration](#component-configuration) section below.
- `BUILD_EVERYTHING`: builds all optional components, overrides other `BUILD_<component>` flags when enabled
- `SERENITY_CACHE_DIR`: sets the location of a shared cache of downloaded files. Should not need to be set unless managing a distribution package.
- `ENABLE_NETWORK_DOWNLOADS`: allows downloading files from the internet during the build. Default on, turning off enables offline builds. For offline builds, the structure of the SERENITY_CACHE_DIR must be set up the way that the build expects.
- `ENABLE_ACCELERATED_GRAPHICS`: builds features that use accelerated graphics APIs to speed up painting and drawing using native graphics libraries. 

Many parts of the codebase have debug functionality, mostly consisting of additional messages printed to the debug console. This is done via the `<component_name>_DEBUG` macros, which can be enabled individually at build time. They are listed in [this file](../Meta/CMake/all_the_debug_macros.cmake).

To toggle or change a build option, see the [CMake Cache Manipulation](#cmake-cache-manipulation) section below.

## CMake Cache Manipulation

CMake caches variables and options in the binary directory. This allows a developer to tailor variables that are `set()` within the persistent configuration cache.

There are three main ways to manipulate the cache:
- `cmake path/to/binary/dir -DVAR_NAME=Value`
- `ccmake` (TUI interface)
- `cmake-gui`

Options can be set via the initial `cmake` invocation that creates the binary directory to set the initial cache for the binary directory.
Once the binary directory exists, any of the three options above can be used to change the value of cache variables.

For example, boolean options such as `ENABLE_<setting>` or `<component_name>_DEBUG` can be enabled with the value `ON` and disabled with `OFF`:

```console
# Reconfigure an existing binary directory with process debug enabled
$ cmake -B Build/x86_64 -DPROCESS_DEBUG=ON
```

For more information on how the CMake cache works, see the CMake guide for [Running CMake](https://cmake.org/runningcmake/). Additional context is available in the CMake documentation for
[variables](https://cmake.org/cmake/help/latest/manual/cmake-language.7.html#variables) and [set()](https://cmake.org/cmake/help/latest/command/set.html#set-cache-entry).

## Component Configuration

For selecting which components of the system to build and install, a helper program, `ConfigureComponents` is available.

It requires `whiptail` as a dependency, which is available on most systems in the `newt` or `libnewt` package. To build and run it, run the following commands from the `Build/x86_64` directory:
```console
$ ninja configure-components
```

This will prompt you which build type you want to use and allows you to customize it by manually adding or removing certain components. It will then run a CMake command based on the selection as well as `ninja clean` and `rm -rf Root` to remove old build artifacts.

## Tests

For information on running host and target tests, see [Running Tests](RunningTests.md). The documentation there also contains useful information for debugging CI test failures.

## Clang-format updates

Some OS distributions don't ship bleeding-edge clang-format binaries. Below are 2 options to acquire an updated clang-format tool, in order of preference:

1) If you have a Debian-based (apt-based) distribution, use the [LLVM apt repositories](https://apt.llvm.org) to install the latest release of clang-format.
2) Compile LLVM from source as described in the LLVM documentation [here](https://llvm.org/docs/GettingStarted.html#compiling-the-llvm-suite-source-code).
