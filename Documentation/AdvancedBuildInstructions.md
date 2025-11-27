# Advanced Build Instructions

This file covers a few advanced scenarios that go beyond what the basic build guide provides.

## Ninja build targets

The `Meta/ladybird.py` script provides an abstraction over the build targets which are made available by CMake. The
following build targets cannot be accessed through the script and have to be used directly by changing the current
directory to `Build/release` and then running `ninja <target>`:

- `ninja check-style`: Runs the same linters the CI does to verify project style on changed files
- `ninja lint-shell-scripts`: Checks style of shell scripts in the source tree with shellcheck
- `ninja all_generated`: Builds all generated code. Useful for running analysis tools that can use compile_commands.json without a full system build

## CMake build options

There are some optional features that can be enabled during compilation that are intended to help with specific types of development work or introduce experimental features. Currently, the following build options are available:
- `ENABLE_ADDRESS_SANITIZER`: builds in runtime checks for memory corruption bugs (like buffer overflows and memory leaks) in Lagom test cases.
- `ENABLE_MEMORY_SANITIZER`: enables runtime checks for uninitialized memory accesses in Lagom test cases.
- `ENABLE_UNDEFINED_SANITIZER`: builds in runtime checks for [undefined behavior](https://en.wikipedia.org/wiki/Undefined_behavior) (like null pointer dereferences and signed integer overflows) in Lagom and Ladybird.
- `UNDEFINED_BEHAVIOR_IS_FATAL`: makes all undefined behavior sanitizer errors non-recoverable. This option reduces the performance overhead of `ENABLE_UNDEFINED_SANITIZER`.
- `ENABLE_COMPILER_EXPLORER_BUILD`: Skip building non-library entities in Lagom (this only applies to Lagom).
- `ENABLE_FUZZERS`: builds [fuzzers](../Meta/Lagom/ReadMe.md#fuzzing) for various parts of the system.
- `ENABLE_FUZZERS_LIBFUZZER`: builds Clang libFuzzer-based [fuzzers](../Meta/Lagom/ReadMe.md#fuzzing) for various parts of the system.
- `ENABLE_FUZZERS_OSSFUZZ`: builds OSS-Fuzz compatible [fuzzers](../Meta/Lagom/ReadMe.md#fuzzing) for various parts of the system.
- `ENABLE_ALL_THE_DEBUG_MACROS`: used for checking whether debug code compiles on CI. This should not be set normally, as it clutters the console output and makes the system run very slowly. Instead, enable only the needed debug macros, as described below.
- `ENABLE_COMPILETIME_FORMAT_CHECK`: checks for the validity of `std::format`-style format string during compilation. Enabled by default.
- `LAGOM_TOOLS_ONLY`: Skips building libraries, utiltis and tests for [Lagom](../Meta/Lagom/ReadMe.md). Mostly only useful for cross-compilation.
- `INCLUDE_WASM_SPEC_TESTS`: downloads and includes the WebAssembly spec testsuite tests. In order to use this option, you will need to install `prettier` and `wabt`. wabt version 1.0.35 or higher is required to pre-process the WebAssembly spec testsuite.
- `INCLUDE_FLAC_SPEC_TESTS`: downloads and includes the xiph.org FLAC test suite.
- `LADYBIRD_CACHE_DIR`: sets the location of a shared cache of downloaded files. Should not need to be set manually unless managing a distribution package.
- `ENABLE_NETWORK_DOWNLOADS`: allows downloading files from the internet during the build. Default on, turning off enables offline builds. For offline builds, the structure of the LADYBIRD_CACHE_DIR must be set up the way that the build expects.
- `ENABLE_CLANG_PLUGINS`: enables Clang plugins which analyze the code for programming mistakes. See [Clang Plugins](#clang-plugins) below.

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
$ cmake -B Build/ladybird -DPROCESS_DEBUG=ON
```

For more information on how the CMake cache works, see the CMake guide for [Running CMake](https://cmake.org/runningcmake/). Additional context is available in the CMake documentation for
[variables](https://cmake.org/cmake/help/latest/manual/cmake-language.7.html#variables) and [set()](https://cmake.org/cmake/help/latest/command/set.html#set-cache-entry).

## Tests

For information on running host and target tests, see [Testing](Testing.md). The documentation there also contains useful information for debugging CI test failures.

## Clang-format updates

Some OS distributions don't ship bleeding-edge clang-format binaries. Below are 2 options to acquire an updated clang-format tool, in order of preference:

1) If you have a Debian-based (apt-based) distribution, use the [LLVM apt repositories](https://apt.llvm.org) to install the latest release of clang-format.
2) Compile LLVM from source as described in the LLVM documentation [here](https://llvm.org/docs/GettingStarted.html#compiling-the-llvm-suite-source-code).

## Clangd Configuration

Clangd will automatically look for configuration information in files
named `.clangd` in each of the parent directories of the file being
edited. The Ladybird source code repository has a top-level `.clangd`
configuration file in the root directory. One of the configuration
stanzas in that file specifies the location for a compilation database.
Depending on your build configuration (e.g., Debug, default, Sanitizer,
etc.), the path to the compilation database in that file may not be
correct. The result is that `clangd` will have a difficult time
understanding all your include directories. To resolve the problem, you
can use the `Meta/configure-clangd.sh` script.

## Clang Plugins

Clang plugins are used to validate the code at compile time. Currently, they are used to detect JavaScript-related
garbage collection faux pas, such as neglecting to visit a garbage-collected type.

In order to enable Clang plugins, you will need Clang's development headers installed. For example, on Ubuntu this is
the `libclang-dev` package.

When Clang plugins are enabled, it is recommended to have the following environment variable set for ccache:

```bash
export CCACHE_COMPILERCHECK="%compiler% -v"
```

By default, ccache will include the plugins themselves in file hashes. So if a plugin changes, the hash of every file
will change, and you will be stuck with an uncached build. This setting will prevent ccache from using plugins in the
file hashes.

## Building the Flatpak

Ladybird has an in-tree Flatpak manifest that can be used to build a Flatpak package for Ladybird. The recommended way
to build the Flatpak is to use the `flatpak-builder` tool, which is available in most distributions. See the
[Flatpak setup documentation](https://flatpak.org/setup/) on how to configure your environment for user Flatpak
builds, and to configure the Flathub repo. The Ladybird Flatpak manifest at
`Meta/CMake/flatpak/org.ladybird.Ladybird.json` can be built with the following command:

```bash
flatpak-builder --user --force-clean --install-deps-from=flathub \
  --ccache --repo=Build/repo --install Build/flatpak \
  Meta/CMake/flatpak/org.ladybird.Ladybird.json 
```

This command will build the Flatpak bundle and install it into the local Flatpak repository at `Build/repo`. Expect this
to take a long time, as it will download and build all the dependencies of Ladybird as well. `flatpak-builder` will drop
caches and build files in `.flatpak-builder`, as well as in the two subfolders `Build/repo` and `Build/flatpak`.

The Flatpak can be run with the following command:

```bash
flatpak run --user org.ladybird.Ladybird
```

In order to debug the Flatpak, you can pass the `--devel` and `--command=sh` flags to `flatpak run`. This will drop you
into a shell in the Flatpak sandbox:

```bash
flatpak run --user --command=sh --devel org.ladybird.Ladybird
```

## Debugging without any optimizations

It’s possible that when trying to inspect certain frame variables in your debugger, you’ll get an error similar to the following:

> error: Couldn't look up symbols: __ZN2AK6Detail10StringBaseD2Ev
> Hint: The expression tried to call a function that is not present in the target, perhaps because it was optimized out by the compiler.

If you do run into such an error, the rest of this section explains how to deal with it.

> [!WARNING]
> You probably only want to make the build-file change described below while you’re in the middle of examining the state of a particular build in your debugger — and then you’ll want to revert it after you’re done debugging. You otherwise probably don’t want to have the build-file change in place while you’re running WPT tests or in-tree tests and checking the results.

1. At your command-line prompt in your shell environment, copy and paste the following:

      ```diff
      $ patch -p1 <<EOF
      diff --git a/Meta/CMake/lagom_compile_options.cmake b/Meta/CMake/lagom_compile_options.cmake
      index 7fec47ac843..45c3af87493 100644
      --- a/Meta/CMake/lagom_compile_options.cmake
      +++ b/Meta/CMake/lagom_compile_options.cmake
      @@ -29,7 +29,7 @@ if (CMAKE_BUILD_TYPE STREQUAL "Debug")
           if (NOT MSVC)
               add_cxx_compile_options(-ggdb3)
           endif()
      -    add_cxx_compile_options(-Og)
      +    add_cxx_compile_options(-O0)
       elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
           add_cxx_compile_options(-O2)
           if (NOT MSVC)
      EOF
      ```
   …or else copy and paste that patch, and apply it in whatever way you normally use for applying patches.

   That will patch the build config in such a way as to disable all compiler optimizations and make all debug symbols always available.

2. At your command-line prompt in your shell environment, run the following command:

      ```
      git update-index --skip-worktree Meta/CMake/lagom_compile_options.cmake
      ```

   That will cause git to ignore the change you made to that build file. Otherwise, if you don’t run that command, git will consider that build file to have been modified, and you might then end up inadvertently committing the changes to that build file as part of some actual code change you’re making to the sources that you’re in the process of debugging.

3. Run a build again with the `Debug` preset, and then go back into your debugger. You’ll now be able to inspect any frame variable that you weren’t able to previously.

After you’ve finished debugging your code changes with that build, you can revert the above changes by doing this:

1. At your command-line prompt in your shell environment, run the following:

      ```
      git update-index --no-skip-worktree Meta/CMake/lagom_compile_options.cmake \
          && git checkout Meta/CMake/lagom_compile_options.cmake
      ```

That will restore your git environment to the state it was in before you patched the build file.

## Building with Swift support

There is experimental Swift 6 support in the Ladybird codebase. This experiment intends to determine whether Swift 6 and
its improved C++ interoperability is a good choice for new memory-safe and concurrent code for Ladybird.

Building with Swift 6 support requires a main snapshot toolchain. The Ladybird team is actively working with the Swift
team to improve the C++ interop features to meet the needs of our project.

The best way to get started is with `swiftly`. After setting up a `swiftly` toolchain, any of the existing build presets
can be modified to use the Swift toolchain. However, note that in order to build Swift support into the project, the
build must use a version of Clang that is built from an LLVM fork with Swift support. The two places this can be found
are from the swift.org snapshot/release toolchains, and Xcode toolchains. Upstream llvm.org Clang does not support
Swift, and GCC does not support Swift either.

### Get Swiftly

`swiftly` is a tool that helps you manage Swift toolchains. It can be installed from https://www.swift.org/install/linux/
or https://www.swift.org/install/macos/ as applicable. After following the instructions on the swift.org install page,
`swiftly` installs the latest release toolchain. If you wish to save space, add the `--skip-install` flag to the `swiftly
init` invocation. If you wish to avoid `swiftly` messing with your shellrc files, add `--no-modify-profile`. On some Linux
platforms, it may be necessary to add a `--platform` flag to the `swiftly init` invocation to instruct `swiftly` on which
supported platform to masquerade as. This is especially necessary on Fedora or other non-Debian based distributions.

Note that while `$SWIFTLY_HOME_DIR` and `$SWIFTLY_BIN_DIR` can be used
to set the installed location of the `swiftly` binary and its associated files, the install location of toolchains is not
nearly as customizable. On Linux they will always be placed in `$XDG_DATA_HOME/swiftly/toolchains`, and on macOS they will always be
placed in `$HOME/Library/Developer/Toolchains`. On macOS, the `.pkg` file will always drop temporary files in `$HOME/.swiftly`,
so be sure to clear them out if you change the default home/bin directories.

### Build with Swift

The simplest way to enable Swift is to use the `Swift_Release` preset and `ladybird.py`.

```bash
./Meta/ladybird.py build --preset Swift_Release
```

Note that because building with Swift support requires use of `clang` and `clang++` from a Swift toolchain, a standard
install of Clang or GCC will not work. Additional IDE settings are be required to ensure that the IDE uses the correct
compiler paths. Trying to use just `clang` or `$SWIFTLY_BIN_DIR/clang` will both fail, due to https://github.com/swiftlang/swiftly/issues/272.

The full paths that must be configured for the C and C++ compilers in your IDE are
`$(swiftly use --print-location)/usr/bin/clang` and `$(swiftly use --print-location)/usr/bin/clang++`. These paths
will change depending on the version of the Swift toolchain specified in `.swift-version`.

As another note, the main-snapshot toolchains from swift.org are `+assertion` builds. This means that both `clang` and
`swiftc` are built with extra assertions that will cause compile-times to be longer than a standard release build.

To configure the build preset manually, you must first install the specified Swift toolchain, and then set the C and C++
compiler paths manually.

```bash
swiftly install

cmake --preset Swift_Release \
   -DCMAKE_C_COMPILER=$(swiftly use --print-location)/usr/bin/clang \
   -DCMAKE_CXX_COMPILER=$(swiftly use --print-location)/usr/bin/clang++
```
