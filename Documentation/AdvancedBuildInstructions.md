# Advanced Build Instructions

This file covers a few advanced scenarios that go beyond what the basic build guide provides.

## Ninja build targets

The `Meta/ladybird.sh` script provides an abstraction over the build targets which are made available by CMake. The
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
- `SERENITY_CACHE_DIR`: sets the location of a shared cache of downloaded files. Should not need to be set manually unless managing a distribution package.
- `ENABLE_NETWORK_DOWNLOADS`: allows downloading files from the internet during the build. Default on, turning off enables offline builds. For offline builds, the structure of the SERENITY_CACHE_DIR must be set up the way that the build expects.
- `ENABLE_CLANG_PLUGINS`: enables clang plugins which analyze the code for programming mistakes.

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
$ cmake -B Build/ladbyird -DPROCESS_DEBUG=ON
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
etc), the path to the compilation database in that file may not be
correct. The result is that clangd will have a difficult time
understanding all your include directories. To resolve the problem, you
can use the `Meta/configure-clangd.sh` script.

## Clang Plugins

Clang plugins are used to validate the code at compile time. Currently, they are used to detect JavaScript-related
garbage collection faux pas, such as neglecting to visit a garbage-collected type.

When clang plugins are enabled, it is recommended to have the following environment variable set for ccache:

```bash
export CCACHE_COMPILERCHECK="%compiler% -v"
```

By default, ccache will include the plugins themselves in file hashes. So if a plugin changes, the hash of every file
will change, and you will be stuck with an uncached build. This setting will prevent ccache from using plugins in the
file hashes.

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
