# Testing Ladybird

Tests are located in `Tests/`, with a directory for each library.

Every feature or bug fix added to LibWeb should have a corresponding test in `Tests/LibWeb`.
The test should be either a Text, Layout, Ref, or Screenshot test depending on the feature.
Tests of internal C++ code go in their own `TestFoo.cpp` file in `Tests/LibWeb`.

## Running Tests

> [!NOTE]
> To reproduce a CI failure, see the section on [Running with Sanitizers](#running-with-sanitizers).

The easiest way to run tests is to use the `ladybird.sh` script. The LibWeb tests are registered with CMake as a test in
`UI/CMakeLists.txt`. Using the built-in test filtering, you can run all tests with `Meta/ladybird.sh test` or run
just the LibWeb tests with `Meta/ladybird.sh test LibWeb`. The second way is to invoke the headless browser test runner
directly. See the invocation in `UI/CMakeLists.txt` for the expected command line arguments.

A third way is to invoke `ctest` directly. The simplest method is to use the `default` preset from ``CMakePresets.json``:

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

### Running with Sanitizers

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

### Running the Web Platform Tests

The Web Platform Tests can be run with the `WPT.sh` script. This script can also be used to compare the results of two
test runs.

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

### Importing Web Platform Tests

You can import certain Web Platform Tests (WPT) tests into your Ladybird clone (if they’re tests of type that can be imported — and especially if any code changes you’re making cause Ladybird to pass any WPT tests it hasn’t yet been passing). Here’s how:

```sh
./Meta/WPT.sh import html/dom/aria-attribute-reflection.html
```

That is, you give `./Meta/WPT.sh import` the path part of any `http://wpt.live/` URL for a WPT test you want to import. It will then download both that test and any of its JavaScript scripts, copy those to the `Tests/LibWeb/<test-type>/input/wpt-import` directory, run the test, and then in the `Tests/LibWeb/<test-type>/expected/wpt-import` directory, it will create a file with the expected results from the test.


## Writing tests

Running `Tests/LibWeb/add_libweb_test.py your-new-test-name test_type` will create a new test HTML file in
`Tests/LibWeb/test_type(/input)` (`/input` is appended for Text and Layout tests) with the correct boilerplate
code for a `test_type` test — along with a corresponding expectations file in the appropriate directory, e.g.,
`Tests/LibWeb/Text/expected/your-new-test-name.txt`, for a Text test, or
`Tests/LibWeb/Ref/reference/your-new-test-name.txt` for a Ref test. The accepted `test_types` are "Text",
"Ref", "Screenshot", and "Layout".

If you make a new Text or Layout test, after you update/replace the generated boilerplate in your
`your-new-test-name.html` test file with your actual test, running
`./Meta/ladybird.sh run headless-browser --run-tests "./Tests/LibWeb" --rebaseline -f Text/input/foobar.html`
will regenerate the corresponding expectations file to match the actual output from your updated test.

If you add a new Ref or Screenshot test, you'll need to supply the equivalently rendering HTML manually.

### Text tests

Text tests are intended to test Web APIs that don't have a visual representation. They are written in JavaScript and
run in a headless browser. Each test has a test function in a script tag that exercises the API and prints expected
results using the `println` function. `println` calls are accumulated into an output test file, which is then
compared to the expected output file by the test runner.

Text tests can be either sync or async. Async tests should use the `done` callback to signal completion.
Async tests are not necessarily run in an async context, they simply require the test function to signal completion
when it is done. If an async context is needed to test the API, the lambda passed to `test` can be async.

### Layout

Layout tests compare the layout tree of a page with an expected one. They are best suited for testing layout code, but
are also used for testing some other features that have an observable effect on the layout. No JavaScript is needed —
once the page loads, the layout tree will be dumped automatically.

### Ref

Reference or "ref" tests compare a screenshot of the test page with one of a reference page. The test passes if the two
are identical. These are ideal for testing visual effects such as background images or shadows. If you're finding it
difficult to recreate the effect in the reference page, (such as for SVG or canvas,) consider using a Screenshot test
instead.

Each Ref test includes a special `<link rel="match" href="../expected/my-test-ref.html" />` tag, which the test runner
uses to locate the reference page. In this way, multiple tests can use the same reference.

### Screenshot

Screenshot tests can be thought of as a subtype of Ref tests, where the reference page is a single `<img>` tag linking
to a screenshot of the expected output. In general, try to avoid using them if a regular Ref test would do, as they are
sensitive to small rendering changes, and won't work on all platforms.

Like Ref tests, they require a `<link rel="match" href="../expected/my-test-ref.html" />` tag to indicate the reference
page to use.
