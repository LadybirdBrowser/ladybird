# Lagom

The Serenity C++ libraries, for other Operating Systems.

## About

If you want to bring the comfortable Serenity classes with you to another system, look no further. This is basically a "port" of the `AK` and `LibCore` libraries to generic \*nix systems.

*Lagom* is a Swedish word that means "just the right amount." ([Wikipedia](https://en.wikipedia.org/wiki/Lagom))

Lagom is used by the Serenity project in the following ways:

- [Build tools](./Tools) required to build Serenity itself using Serenity's own C++ libraries are in Lagom.
- [Unit tests](../../Documentation/RunningTests.md) in CI are built using the Lagom build for host systems to ensure portability.
- [Continuous fuzzing](#fuzzing-on-oss-fuzz) is done with the help of OSS-fuzz using the Lagom build.
- [The Ladybird browser](../../Ladybird/README.md) uses Lagom to provide LibWeb and LibJS for non-Serenity systems.
- [ECMA 262 spec tests](https://ladybirdbrowser.github.io/libjs-website/test262) for LibJS are run per-commit and tracked on [LibJS website](https://ladybirdbrowser.github.io/libjs-website/).
- [Wasm spec tests](https://ladybirdbrowser.github.io/libjs-website/wasm) for LibWasm are run per-commit and tracked on [LibJS website](https://ladybirdbrowser.github.io/libjs-website/).

## Using Lagom in an External Project
It is possible to use Lagom for your own projects outside of Serenity too!

An example of this in use can be found in the [LibJS test262 runner](https://github.com/SerenityOS/libjs-test262).

To implement this yourself:
- Download a copy of [SerenityOS/libjs-test262/cmake/FetchLagom.cmake](https://github.com/SerenityOS/libjs-test262/blob/7832c333c1504eecf1c5f9e4247aa6b34a52a3be/cmake/FetchLagom.cmake) and place it wherever you wish
- In your root `CMakeLists.txt`, add the following commands:
  ```cmake
  include(FetchContent)
  include(cmake/FetchLagom.cmake) # If you've placed the file downloaded above differently, be sure to reflect that in this command :^)
  ```
- In addition, you will need to also add some compile options that Serenity uses to ensure no warnings or errors:
  ```cmake
  add_compile_options(-Wno-literal-suffix) # AK::StringView defines operator""sv, which GCC complains does not have an underscore.
  add_compile_options(-fno-gnu-keywords)   # JS::Value has a method named typeof, which also happens to be a GNU keyword.
  ```

Now, you can link against Lagom libraries.

Things to keep in mind:
- You should prefer to use a library's `Lagom::` alias when linking
  - Example: `Lagom::Core` vs `LibCore`
- If you still _need_ to use the C++ standard library, you may have to compile with the `AK_DONT_REPLACE_STD` macro.
  - Serenity defines its own `move` and `forward` functions inside of `AK/StdLibExtras.h` that will clash with the standard library's definitions. This macro will make Serenity use the standard library's `move` and `forward` instead.
- If your application has name clashes with any names in AK, you may have to define `USING_AK_GLOBALLY=0` for the files that have visibility to both sets of headers.

## Fuzzing

Lagom can be used to fuzz parts of SerenityOS's code base. Fuzzers can be run locally, and they also run continuously on OSS-Fuzz.

### Fuzzing locally

Lagom can be used to fuzz parts of SerenityOS's code base. This requires building with `clang`, so it's convenient to use a different build directory for that. Fuzzers work best with Address Sanitizer enabled. The fuzzer build requires code generators to be pre-built without fuzzing in a two stage build.

To build with LLVM's libFuzzer, invoke the ``BuildFuzzers.sh`` script with no arguments.

```sh
./BuildFuzzers.sh
./Build/lagom-fuzzers/FuzzSomething # The full list can be found in Fuzzers/CMakeLists.txt
```

(Note that we require clang >= 14, see the pick_clang() function in the script for the paths that are searched)

To build fuzzers without any kind of default instrumentation, pass the ``--standalone`` flag to ``BuildFuzzers.sh``:

```sh
./BuildFuzzers.sh --standalone

# This binary will read a single test input from a given filename (or, if no filename is given, from stdin) and exit.
./Build/lagom-fuzzers-standalone/Fuzzers/FuzzSomething
```

The fuzzing build's CMake cache can be manipulated with commands like `cmake -B Build/fuzzers -S . -DENABLE_LAGOM_CCACHE=OFF`.

Any fuzzing results (particularly slow inputs, crashes, etc.) will be dropped in the current directory.

Fuzzers work better if you give them a fuzz corpus, e.g. `./Fuzzers/FuzzBMPLoader ../Base/res/html/misc/bmpsuite_files/rgba32-61754.bmp` Pay attention that LLVM also likes creating new files, don't blindly commit them (yet)!

To run several fuzz jobs in parallel, pass `-jobs=24 -workers=24`.

To get less log output, pass `-close_fd_mask=3` -- but that but hides assertion messages. Just `1` only closes stdout.
It's good to move overzealous log output behind `FOO_DEBUG` macros.

Using other fuzzers is possible, as demonstrated by the OSS-fuzz build. Doing so likely requires setting CFLAGS and CXXFLAGS
on the second stage of the CMake build, or in your environment.

### Keeping track of interesting testcases

There are many quirky files that exercise a lot of interesting edge cases.
We should probably keep track of them, somewhere.

We have a [bmp suite and a jpg suite and several others](https://github.com/LadybirdWebBrowser/ladybird/tree/master/Base/res/html/misc).
They are GPL'ed, and therefore not quite as compatible with the rest of Serenity.
That's probably not a problem, but keeping "our" testcases separate from those GPL'ed suits sounds like a good idea.

We could keep those testcases somewhere else in the repository, like a `fuzz` directory.
But fuzzing tends to generate more and more and more files, and they will blow up in size.
Especially if we keep all interesting testcases, which is exactly what I intend to do.

So we should keep the actual testcases out of the main serenity repo,
that's why we created https://github.com/SerenityOS/serenity-fuzz-corpora

Feel free to upload lots and lots files there, or use them for great good!

### Fuzzing on OSS-Fuzz

https://oss-fuzz.com/ automatically runs all fuzzers in the Fuzzers/ subdirectory whose name starts with "Fuzz" and which are added to the build in `Fuzzers/CMakeLists.txt` if `ENABLE_FUZZERS_OSSFUZZ` is set. Looking for "serenity" on oss-fuzz.com finds interesting links, in particular:

* [known open bugs found by fuzzers](https://oss-fuzz.com/testcases?project=serenity&open=yes)
  * [oss-fuzz bug tracker for these](https://bugs.chromium.org/p/oss-fuzz/issues/list?sort=-opened&can=1&q=proj:serenity)
* [coverage report](https://oss-fuzz.com/coverage-report/job/libfuzzer_asan_serenity/latest)
* [build logs](https://oss-fuzz-build-logs.storage.googleapis.com/index.html#serenity)

Here's [Serenity's OSS-Fuzz Config](https://github.com/google/oss-fuzz/tree/master/projects/serenity). The configuration runs the `BuildFuzzers.sh` script with the `--oss-fuzz` argument inside the OSS-Fuzz docker container.

To run the OSS-fuzz build locally:

```
git clone https://github.com/google/oss-fuzz/
cd oss-fuzz
python3 infra/helper.py build_image serenity
python3 infra/helper.py build_fuzzers serenity
```

These commands will put the fuzzers in `build/out/serenity` in the oss-fuzz repo. You can run the binaries in there individually, or simply type:

```
python3 infra/helper.py run_fuzzer serenity FUZZER_NAME
```

To build the fuzzers using the oss-fuzz build process, but against a local serenity checkout:

```
python3 infra/helper.py build_fuzzers serenity $HOME/src/serenity/
```

To run a shell in oss-fuzz's serenity docker image:

```
docker run -it gcr.io/oss-fuzz/serenity bash
```

### Analyzing a crash

LLVM fuzzers have a weird interface. In particular, to see the help, you need to call it with `-help=1`, and it will ignore `--help` and `-help`.

To reproduce a crash, run it like this: `MyFuzzer crash-27480a219572aa5a11b285968a3632a4cf25388e`

To reproduce a crash in gdb, you want to disable various signal handlers, so that gdb sees the actual location of the crash:

```
$ gdb ./Fuzzers/FuzzBMP
<... SNIP some output ...>
(gdb) run -handle_abrt=0 -handle_segv=0 crash-27480a219572aa5a11b285968a3632a4cf25388e
<... SNIP some output ...>
FuzzBMP: ../../Userland/Libraries/LibGfx/Bitmap.cpp:84: Gfx::Bitmap::Bitmap(Gfx::BitmapFormat, const Gfx::IntSize &, Gfx::Bitmap::Purgeable): Assertion `m_data && m_data != (void*)-1' failed.

Thread 1 "FuzzBMP" received signal SIGABRT, Aborted.
__GI_raise (sig=sig@entry=6) at ../sysdeps/unix/sysv/linux/raise.c:50
50	../sysdeps/unix/sysv/linux/raise.c: File or directory not found.
(gdb)
```

UBSan doesn't always give useful information. use something like `export UBSAN_OPTIONS=print_stacktrace=1` to always print stacktraces.

You may run into annoying issues with the stacktrace:

```
==123456==WARNING: invalid path to external symbolizer!
==123456==WARNING: Failed to use and restart external symbolizer!
```

That means it couldn't find the executable `llvm-symbolizer`, which could be in your OS's package `llvm`.
`llvm-symbolizer-11` will [not be recognized](https://stackoverflow.com/a/42845444/).
