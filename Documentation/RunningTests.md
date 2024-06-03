# Running SerenityOS Tests

There are two classes of tests built during a SerenityOS build: host tests and target tests. Host tests run on the build
machine, and use [Lagom](../Meta/Lagom/ReadMe.md) to build SerenityOS userspace libraries for the host platform. Target
tests run on the SerenityOS machine, either emulated or bare metal.

## Running Host Tests

There are two ways to build host tests: from a full build, or from a Lagom-only build. The only difference is the CMake
command used to initialize the build directory.

For a full build, pass `-DBUILD_LAGOM=ON` to the CMake command.

```sh
cmake -GNinja -S Meta/CMake/Superbuild -B Build/superbuild-x86_64 -DBUILD_LAGOM=ON
```

For a Lagom-only build, pass the Lagom directory to CMake. The `BUILD_LAGOM` CMake option is still required.

```sh
cmake -GNinja -S Meta/Lagom -B Build/lagom -DBUILD_LAGOM=ON
```

In both cases, the tests can be run via ninja after doing a build. Note that `test-js` requires the `LADYBIRD_SOURCE_DIR` environment variable to be set
to the root of the serenity source tree when running on a non-SerenityOS host.

```sh
# /path/to/serenity repository
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

### Running with Sanitizers

CI runs host tests with Address Sanitizer and Undefined Sanitizer instrumentation enabled. These tools catch many
classes of common C++ errors, including memory leaks, out of bounds access to stack and heap allocations, and
signed integer overflow. For more info on the sanitizers, check out the Address Sanitizer [wiki page](https://github.com/google/sanitizers/wiki),
or the Undefined Sanitizer [documentation](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html) from clang.

Note that a sanitizer build will take significantly longer than a non-sanitizer build, and will mess with caches in tools such as `ccache`.
The sanitizers can be enabled with the `-DENABLE_FOO_SANITIZER` set of flags. For the Serenity target, only the Undefined Sanitizers is supported.

```sh
cmake -GNinja -S Meta/Lagom -B Build/lagom -DBUILD_LAGOM=ON -DENABLE_ADDRESS_SANITIZER=ON -DENABLE_UNDEFINED_SANITIZER=ON
cd Build/lagom
ninja
CTEST_OUTPUT_ON_FAILURE=1 LADYBIRD_SOURCE_DIR=${PWD}/../.. ninja test
```

To ensure that Undefined Sanitizer errors fail the test, the `halt_on_error` flag should be set to 1 in the environment variable `UBSAN_OPTIONS`.

```sh
UBSAN_OPTIONS=halt_on_error=1 CTEST_OUTPUT_ON_FAILURE=1 LADYBIRD_SOURCE_DIR=${PWD}/.. ninja test
```

## Running Target Tests

Tests built for the SerenityOS target get installed either into `/usr/Tests` or `/bin`. `/usr/Tests` is preferred, but
some system tests are installed into `/bin` for historical reasons.

The easiest way to run all of the known tests in the system is to use the `run-tests-and-shutdown.sh` script that gets
installed into `/home/anon/Tests`. When running in CI, the environment variable `$DO_SHUTDOWN_AFTER_TESTS` is set, which
will run `shutdown -n` after running all the tests.

For completeness, a basic on-target test run will need the SerenityOS image built and run via QEMU.

```sh
cmake -GNinja -S Meta/CMake/Superbuild -B Build/superbuild-x86_64
cmake --build Build/superbuild-x86_64
cd Build/x86_64
ninja install && ninja qemu-image && ninja run
```

In the initial terminal, one can easily run the test runner script:

```
courage ~ $ ./Tests/run-tests-and-shutdown.sh
=== Running Tests on SerenityOS ===
...
```

CI runs the tests in self-test mode, using the 'ci' run options and the TestRunner entry in /etc/SystemServer.ini to run
tests automatically on startup.

The system server entry looks as below:

```ini
[TestRunner@ttyS0]
Executable=/home/anon/Tests/run-tests-and-shutdown.sh
StdIO=/dev/ttyS0
Environment=DO_SHUTDOWN_AFTER_TESTS=1 TERM=xterm PATH=/usr/local/bin:/usr/bin:/bin
User=anon
WorkingDirectory=/home/anon
SystemModes=self-test
```

`/dev/ttyS0` is used as stdio because that serial port is connected when qemu is run with `-display none` and
`-serial stdio`, and output to it will show up in the stdout of the qemu window. Separately, the CI run script redirects
the serial debug output to `./debug.log` so that both stdout of the tests and the dbgln from the kernel/tests can be
captured.

To run with CI's TestRunner system server entry, SerenityOS needs booted in self-test mode. Running the following shell
lines will boot SerenityOS in self-test mode, run tests, and exit. Note that CI also sets `panic=shutdown` to terminate qemu;
the default value `halt` keeps qemu around, which allows you to inspect the state.

```sh
export SERENITY_RUN=ci
export SERENITY_KERNEL_CMDLINE="graphics_subsystem_mode=off system_mode=self-test"
ninja run
```
