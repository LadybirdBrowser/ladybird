# Running Tests

To reproduce a CI failure, see the section on [Running with Sanitizers](#running-with-sanitizers).

The simplest way to run tests locally is to use the `default` preset from ``CMakePresets.json``:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

If you want to avoid building and running LibWeb tests, you can use a Lagom-only build.

```sh
cmake -GNinja -S Meta/Lagom -B Build/lagom
```

The tests can be run via ninja after doing a build. Note that `test-js` requires the `LADYBIRD_SOURCE_DIR` environment variable to be set
to the root of the ladybird source tree.

```sh
# /path/to/ladybird repository
export LADYBIRD_SOURCE_DIR=${PWD}
cd Build/lagom
ninja
ninja test
```

To see the stdout/stderr output of failing tests, the recommended way is to set the environment variable [`CTEST_OUTPUT_ON_FAILURE`](https://cmake.org/cmake/help/latest/manual/ctest.1.html#options) to 1.

```sh
CTEST_OUTPUT_ON_FAILURE=1 ninja test

# or, using ctest directly...
ctest --output-on-failure
```

# Running with Sanitizers

CI runs host tests with Address Sanitizer and Undefined Sanitizer instrumentation enabled. These tools catch many
classes of common C++ errors, including memory leaks, out of bounds access to stack and heap allocations, and
signed integer overflow. For more info on the sanitizers, check out the Address Sanitizer [wiki page](https://github.com/google/sanitizers/wiki),
or the Undefined Sanitizer [documentation](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html) from clang.

Note that a sanitizer build will take significantly longer than a non-sanitizer build, and will mess with caches in tools such as `ccache`.
The sanitizers can be enabled with the `-DENABLE_FOO_SANITIZER` set of flags.

The simplest way to enable sanitizers is to use the `Sanitizer` preset.

```sh
cmake --preset Sanitizer
cmake --build --preset Sanitizer
ctest --preset Sanitizer
```

Or from a Lagom build:

To ensure that the test behaves the same way as CI, make sure to set the ASAN_OPTIONS and UBSAN_OPTIONS appropriately.
The Sanitizer test preset already sets these environment variables.

```sh
export ASAN_OPTIONS='strict_string_checks=1:check_initialization_order=1:strict_init_order=1:detect_stack_use_after_return=1:allocator_may_return_null=1'
export UBSAN_OPTIONS='print_stacktrace=1:print_summary=1:halt_on_error=1'
cmake -GNinja -S Meta/Lagom -B Build/lagom -DENABLE_ADDRESS_SANITIZER=ON -DENABLE_UNDEFINED_SANITIZER=ON
cd Build/lagom
ninja
CTEST_OUTPUT_ON_FAILURE=1 LADYBIRD_SOURCE_DIR=${PWD}/../.. ninja test
```

# Running the Web Platform Tests

The Web Platform Tests can be run with the `WPT.sh` script. This script can also be used to compare the results of two 
test runs.

Enabling the Qt chrome is recommended when running the Web Platform Tests on MacOS. This can be done by running the 
following command:

```sh
cmake -GNinja Build/ladybird -DENABLE_QT=ON
```

Example usage:

```sh
# Run the WPT tests then run them again, comparing the results from the two runs
./Meta/WPT.sh run --log expectations.log css
git checkout my-css-change
./Meta/WPT.sh compare --log results.log expectations.log css
```

```sh
# Pull the latest changes from the upstream WPT repository
./Meta/WPT.sh update
# Run all of the Web Platform Tests, outputting the results to results.log
./Meta/WPT.sh run --log results.log 
```
