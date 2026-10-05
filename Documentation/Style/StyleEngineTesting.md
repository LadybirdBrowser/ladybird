# StyleEngine: testing and debugging

How to test style engine changes, how the verification machinery works, and how to debug. The engine itself is described in [StyleEngine.md](StyleEngine.md).

The correctness bar: **incremental results must be identical to full recomputation from authoritative inputs.** Over-invalidation (recomputing something that did not change) is a performance problem; under-invalidation (missing something that did change) is always a bug.

## 1. The test stack

Run these from fastest to slowest; a style change is not done until all of them pass.

**Rust unit and differential tests.**

```sh
cd Libraries/LibWeb/Rust && cargo test --release -p libweb_rust
```

`tests.rs` drives engine operators directly through the same entry points the bridge uses. `differential_tests.rs` compares incremental evaluation against the exact cold evaluator over generated mutation sequences.

**Web tests.** The style-engine and style-invalidation suites under `Tests/LibWeb/Text/input/css/` assert observable behavior (computed styles after mutations) and, via internals hooks, engine behavior (recompute counts, invalidation reach). The full LibWeb suite is the outer gate:

```sh
./bin/test-web
```

**Verify mode.** `--verify-style` enables every style verification gate for the run (it sets the `LIBWEB_VERIFY_*` environment variables below before spawning WebContent):

```sh
./bin/test-web --verify-style
```

A focused loop for style work:

```sh
./bin/test-web --verify-style -f Text/input/css/style-engine/ -f Text/input/css/style-invalidation/
```

The gates are per-mechanism checks that run at their mechanism's site. Some re-derive incremental results through the exact cold evaluator and compare (`LIBWEB_VERIFY_STYLE_ANSWER_PATCH`, `LIBWEB_VERIFY_SELECTOR_TRUTH_DERIVATION`, `LIBWEB_VERIFY_CASCADE_WINNERS`); others assert structural properties (`LIBWEB_VERIFY_STYLE_PLAN_PROVENANCE`, `LIBWEB_VERIFY_PUBLISHED_STYLE_TRANSACTION`), and two C++-side gates cover the computed closure and the style-diff fast path (`LIBWEB_VERIFY_COMPUTED_CLOSURE`, `LIBWEB_VERIFY_STYLE_DIFF_FAST_PATH`).

`LIBWEB_VERIFY_PREFIX_RELATION` compares every live element's maintained prefix memberships with scalar prefix matching after construction and updates. It also verifies retained membership storage accounting, and runs unconditionally in Rust unit tests.

Verification is **observer-only**: structural checks receive an immutable engine view, while checks that need the exact cold evaluator receive a dedicated verifier capability whose only operations perform comparisons. Neither API exposes cache publication or general engine mutation, and every gate returns unit, so a verifier cannot steer engine behavior. A verifier must never disable or bypass the fast path it is checking, and an incomplete comparison is a failure, not a skip.

## 2. Debugging playbook

**A page styles wrong.**

1. Reduce to a test under `Tests/LibWeb/Text/input/css/style-engine/` if you can; run it under `--verify-style`. If verify mode flags it, the divergence report names the node and the stage (match answer, winner), which usually identifies the responsible mechanism.
2. If verify passes but the page is still wrong, the bug is upstream of the engine (input collection, C++ integration) or downstream (consumers): check that the mutation reached the engine as a typed delta, and that consumers read the published record.
3. The per-mechanism `LIBWEB_VERIFY_*` gates bisect between retained-state mechanisms when the full verify pass is too coarse.

**Performance.** A browser profile shows C++ materialization cost and engine cost side by side. The counters (§16 of StyleEngine.md) are exposed through the internals object (`--expose-internals-object` works on live sites via WebDriver) and answer the first triage question: how many reactions and recomputations did this interaction produce, and how many computed styles changed? A large gap is amplification, and the fix is precision, not micro-optimization. Performance claims are settled by measuring the real workload A/B; counter improvements and synthetic benchmarks are supporting evidence, not the verdict.

**Suspected memory-accounting drift.** Every cache and arena reports exact bytes (invariant 8). Capacity tests assert charges reach zero after eviction.

## 3. Writing tests

* Behavior fixes land with a test that failed before the fix. For engine-internal properties (invalidation reach, recompute counts, retention), assert through the internals hooks rather than timing; timing-based tests are forbidden.
* Structural refactors that claim "no behavior change" are proven by the standard stack, not by new tests that would only assert the refactor's own shape.
* When a test needs the engine to be in a specific retained state (warm caches, retained answers), drive it there through ordinary DOM/CSSOM operations in the test body; tests never reach into engine internals to preload state.
