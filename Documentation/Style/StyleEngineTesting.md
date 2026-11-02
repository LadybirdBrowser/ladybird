# Testing and debugging the style engine

[StyleEngine.md](StyleEngine.md) describes the engine.

The correctness bar: **what an incremental update installs must equal what a cold pass over the same inputs computes.**
Over-invalidation costs time and is not a correctness bug. Missed invalidation is.

## Tests

**Rust unit tests** are in `Libraries/LibWeb/Rust/src/css/style/`. `tests.rs` drives the engine through the entry points the
bridge uses, and `differential_tests.rs` compares restyles with cold passes over generated rules and mutations. They run in
the `RustWorkspace` ctest, from the build directory. Do not run Cargo from the repository root.

**Web tests** under `Tests/LibWeb/Text/input/css/` assert behavior: computed styles after mutations.

```sh
./bin/test-web --filter '*css*' --filter '*CSS*'
```

**Memory budgets** under `css/style-memory/` print whether each owner stays within its budget per element, rule and document.

**`--verify-style`** runs the tests with the self-check on:

```sh
./bin/test-web --verify-style --filter '*css*'
```

## Debug switches

| Environment variable | What it does |
| --- | --- |
| `LIBWEB_STYLE_SELFCHECK` | After each update that installed styles, compares the engine's state with a cold reference pass on a fork of the engine. `report` prints differences, and any other value but `0` aborts on them. `threads` also compares cold passes on several threads. |
| `LIBWEB_STYLE_AUDIT_CACHES` | Checks what the traversal's caches answered against computing it. |
| `LIBWEB_VERIFY_STYLE_CHUNKS` | Checks the reads recorded for shared group chunks. |
| `LIBWEB_STYLE_THREADS` | Sets the number of threads a pass uses. |

`internals.styleColdPass()` runs a cold pass and reports on it, and `internals.styleLiveReport()` reports on the live engine.

## Debugging

**A page styles wrong.** Reduce it to a test and run it with `LIBWEB_STYLE_SELFCHECK=report`. A difference there means the update
missed something: the report names the element and what differs. If the self-check passes, the bug is outside the
incremental path: in what the host publishes, in computation itself (a cold pass is wrong too), or in the consumer.

**Performance.** A profile shows host and engine cost side by side. `internals.styleCounters()` says how much work an update
did, and `Meta/StyleUpdateCapture/` captures it for a page.

## Writing tests

- A behavior fix lands with a test that failed before it.
- To assert invalidation reach, compare counters around the mutation rather than timing it.
- Reach a retained state, such as warm caches, through ordinary DOM and CSSOM operations in the test.
