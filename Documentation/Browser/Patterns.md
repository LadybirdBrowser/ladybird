# LibWeb Code Style & Patterns

This document aims to describe agreed upon code style and patterns used across LibWeb.

## Directory Structure

Generally we use a subdirectory, and thus C++ namespace, for each individual spec. For example XHR
(`xhr.spec.whatwg.org`):

-   Code lives in `LibWeb/XHR/`
-   Uses the C++ namespace `Web::XHR`

If necessary, code can also be grouped into sub-subdirectories (for example `HTML/Scripting/`).

Sometimes a spec document affects several areas of the web platform. An example of this is CSSOM,
which of course contains features belonging in `LibWeb/CSS/` / `Web::CSS`, but at the same time
implements additions to `Window` from the HTML spec. Use best judgement in those cases.

## Error Handling

The following error types are commonly used in LibWeb, each having a different purpose:

### `AK::ErrorOr<T>`

This error type is generally only used to propagate OOM errors from AK and other general libraries.
It should not be used to propagate any other kinds of errors, even though this is supported and used
in other parts of the system. For LibWeb, use any of the error types below.

This should be propagated as far as possible before being turned into a JS error object (via the
`TRY_OR_THROW_OOM` macro). This avoids a situation where almost everything returns a generic
`WebIDL::ExceptionOr<T>`.

### `Web::WebIDL::ExceptionOr<T>`

This is the most common and at the same time most broad error type in LibWeb. Internally it stores a
variant of supported errors:

-   `SimpleException`
-   `JS::NonnullGCPtr<DOMException>`
-   `JS::Completion` (from `JS::ThrowCompletionOr<T>`, assumed to be of `Type::Throw`)

Use this error type for anything that needs to interact with the JS bindings, which will generally
know how to turn any of the internally supported errors into JS objects.

### `Web::WebIDL::SimpleException`

This is a thin wrapper around various built-in errors from ECMAScript:

-   `EvalError`
-   `RangeError`
-   `ReferenceError`
-   `TypeError`
-   `URIError`

Instead of constructing one of these directly, create a `SimpleException` with the appropriate type
and message instead whenever required by a web spec. These will be converted into actual JS objects
in the bindings layer.

> **Note** Relevant WebIDL documentation: https://webidl.spec.whatwg.org/#dfn-simple-exception

### `Web::WebIDL::DOMException`

This is an error type from the WebIDL spec specifically for web AOs where none of the JS built-in
error types are sufficient. Like `SimpleException`, use when indicated by a web spec.

> **Note** Relevant WebIDL documentation: https://webidl.spec.whatwg.org/#idl-DOMException

### `JS::ThrowCompletionOr<T>`

This is an error type from LibJS, which uses "completions" to propagate errors and other types of AO
results. Don't use this in LibWeb unless absolutely necessary, e.g. when overriding a `JS::Object`
virtual method that returns this type.

At the call site, these should be wrapped in a `ExceptionOr<T>` as soon as possible for further
propagation.

> **Note** Relevant ECMAScript documentation:
> https://tc39.es/ecma262/#sec-completion-record-specification-type

## Comments

As in LibJS, **all** functions that represent an operation or JS function from a web specification
must have:

-   A spec link, above the function definition:

    ```cpp
    // https://fetch.spec.whatwg.org/#concept-fetch
    WebIDL::ExceptionOr<JS::NonnullGCPtr<Infrastructure::FetchController>> fetch(JS::Realm& realm, Infrastructure::Request& request, Infrastructure::FetchAlgorithms const& algorithms, UseParallelQueue use_parallel_queue)
    {
        // ...
    }
    ```

-   Comments for each individual step of the operation:

    ```cpp
    // 1. Assert: request’s mode is "navigate" or processEarlyHintsResponse is null.
    VERIFY(request.mode() == Infrastructure::Request::Mode::Navigate || !algorithms.process_early_hints_response().has_value());

    // 2. Let taskDestination be null.
    JS::GCPtr<JS::Object> task_destination;

    // ...
    ```

    -   If a step cannot be implemented at the time, prepend it with a `FIXME`
    -   Add a blank line between code and the next comment

-   Optimizations (e.g. fast paths) should be marked as such with an `// OPTIMIZATION:` comment
    explaining the reasoning

-   When adding non-standard code for a feature that is otherwise well-specified, it should be
    marked as such. This does not universally apply as certain areas (layout and painting, for
    instance) are only broadly spec'd

-   If the spec has additional prose before or after its algorithm steps, that doesn't need to be
    copied into the code

## JS Interfaces

### IDL

Try to copy IDL verbatim, only making changes where necessary (IDL parser shortcomings, non-standard
extended attributes).

This includes not reordering functions, changing parameter names, etc.

The major difference to the spec is that we use four spaces for indentation, like for any other code
(not two).

### C++ Naming

Try to stick with the interface's exact name as much as possible for the class and file names (no
`Object` suffixes like in LibJS, for example). When there's a name clash, try to introduce a nested
namespace, e.g. `Fetch::Request` vs `Fetch::Infrastructure::Request`.

### File placement

The `.cpp`, `.h`, and `.idl` files for a given interface should all be in the same directory, unless
the implementation is hand-written when it cannot be generated from IDL. In those cases, no IDL file
is present and code should be placed in `Bindings/`.

## Testing

Every feature or bug fix added to LibWeb should have a corresponding test in `Tests/LibWeb`.
The test should be either a Text, Layout or Ref test depending on the feature.

LibWeb tests can be run in one of two ways. The easiest is to use the `ladybird.sh` script. The LibWeb tests are
registered with CMake as a test in `Ladybird/CMakeLists.txt`. Using the builtin test filtering, you can run all tests
with `Meta/ladybird.sh test` or run just the LibWeb tests with `Meta/ladybird.sh test LibWeb`. The second
way is to invoke the headless browser test runner directly. See the invocation in `Ladybird/CMakeLists.txt` for the
expected command line arguments.

Running `Tests/LibWeb/add_libweb_test.py your-new-test-name` will create a new test HTML file in
`Tests/LibWeb/Text/input/your-new-test-name.html` with the correct boilerplate code for a Text test — along with
a corresponding expectations file in `Tests/LibWeb/Text/expected/your-new-test-name.txt`.

After you update/replace the generated boilerplate in your `your-new-test-name.html` test file with your actual test,
running `./Meta/ladybird.sh run headless-browser --run-tests "${LADYBIRD_SOURCE_DIR}/Tests/LibWeb" --rebaseline -f Text/input/foobar.html` will
regenerate the corresponding expectations file — to match the actual output from your updated test (where
`/opt/ladybird` should be replaced with the absolute path your ladybird clone in your local environment).

Future versions of the `add_libweb_test.py` script will support Layout and Ref tests.

### Text tests

Text tests are intended to test Web APIs that don't have a visual representation. They are written in JavaScript and
run in a headless browser. Each test has a test function in a script tag that exercises the API and prints expected
results using the `println` function. `println` calls are accumulated into an output test file, which is then
compared to the expected output file by the test runner.

Text tests can be either sync or async. Async tests should use the `done` callback to signal completion.
Async tests are not necessarily run in an async context, they simply require the test function to signal completion
when it is done. If an async context is needed to test the API, the lambda passed to `test` can be async.
