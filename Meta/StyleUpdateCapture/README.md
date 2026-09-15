# Style-update capture

`Meta/style-update-capture.py` drives a built headless `WebDriver` over a deterministic
workload and records, per interval, the style engine's phase clocks, its physical-work
counters, the browser's CPU and resident set, and the build's provenance. It uses only the
Python standard library and commands that Linux and macOS both provide, so a capture taken
on one of them reads the same on the other.

`test_capture.py` covers the arithmetic, the platform parsing and the comparison output
without launching a browser:

```
Meta/StyleUpdateCapture/test_capture.py
```

## What the engine publishes

Two ledgers, each additive on its own and never to be summed with the other.

`internals.styleEngineCounters()` reports cumulative microseconds at the transaction
boundary: `transactionMicroseconds` is exactly the sum of `commitMicroseconds`,
`routingPlanningMicroseconds`, `matchingCascadeMicroseconds`,
`computationPublicationMicroseconds`, `emitMicroseconds` and
`transactionRemainderMicroseconds`.

`internals.getStyleInvalidationCounters()` reports the C++ side, where
`styleUpdateMicroseconds` is the sum of `styleUpdateSubmissionMicroseconds`,
`styleUpdateBridgeMicroseconds`, `styleUpdateApplyMicroseconds` and
`styleUpdateRemainderMicroseconds`. The transaction clocks above subdivide
`styleUpdateBridgeMicroseconds`, so adding the two ledgers double-counts the engine.

Alongside the clocks, the same ledgers carry the physical-work counters: how many nodes the
answer pass reached, how many cold matching rows and candidate checks ran, how many record
drives started, how many longhand evaluations were physically executed as opposed to
accepted, and how many drive table slots were copied. A drive that borrows its base copies no
slots, so that last field reads zero; the copies a published result still makes are counted
by the FFI diagnostics counters `longhandTableCopiedSlots` and `longhandTableCopyRetains`.
These are counts of work performed, not of output kept, and they do not depend on how fast
the machine is.

A finer set of pass clocks splits each phase into the passes a transaction actually runs
(routing setup, the routing input loop, sequence routing, the pending-route flush, batch
compilation, the winner-group version advance, the retained-answer patch, completion and the
computation loop). Reading a monotonic clock twice per pass is itself measurable on a
document that flushes often, so those are off unless `LIBWEB_STYLE_PASS_CLOCKS` is set in the
browser's environment. The phase clocks and the work counters are always on.

Documentation/Style/StyleEngine.md describes each counter and the boundary it measures.

## Reading a capture

A capture is one JSON document. `runs[]` holds one entry per configuration and repetition;
each `samples[]` entry is one interval, with:

* `kind` — `setup`, `mutation`, `other` for the flushes between mutations, and the
  `run-total` and `suite-total` summary rows, which overlap the detail rows beneath them.
* `before` and `after` — the raw ledger snapshots that bracket the interval.
* `counters` — their difference, per lane, including the physical-work fields.
* `metrics` — the flattened numbers the comparison reports, keyed `rust.<clock>` and
  `cpp.<clock>`, plus `mutationMicroseconds`, `observationMicroseconds`, `cpuMicroseconds`
  and `rssBytes`.

The capture refuses any interval whose clocks are not additive, so a ledger that does not
partition is a failure at capture time rather than a misleading number later.

`compare` prints median, 95th percentile and maximum per metric and interval, the counter
fields whose values differ, and the boundaries the capture does not measure:

```
Meta/style-update-capture.py compare before.json after.json --summary
```

Process CPU and RSS come from `ps` and carry its resolution. RSS is the resident set at the
interval's endpoints, not a kernel-tracked peak. CPU has one-second resolution on Linux and
hundredths on macOS, so it is readable across a whole run and not across a single mutation.
Both include everything the browser did in the interval, not style alone. Whatever a platform
cannot supply is recorded as null and reported as `UNMEASURED`, never as zero.

## Running it

On the fixture, which is the same file the `style-update-fixture` web test uses, so a change
that breaks the workload breaks a test rather than quietly changing a measurement:

```
Meta/style-update-capture.py capture connected.json --runs 3 --scenario connected
Meta/style-update-capture.py capture detached.json --runs 3 --scenario detached
```

On StyleBench, serve an unmodified checkout yourself and pass its URL. The capture observes
the benchmark through its own runner boundaries and changes no part of its workload:

```
python3 -m http.server 8000 --directory /path/to/StyleBench
Meta/style-update-capture.py capture bench.json --stylebench-url http://localhost:8000/
```

Each configuration and repetition gets a fresh browser profile, and a capture refuses to
overwrite an existing output file or to start while another browser or test process is
running.

## Comparing two builds

Measure two builds against each other in one sitting rather than against a number remembered
from another day. Machines drift; a paired capture does not care.

Build the two versions into separate trees, or keep two sets of the built shared libraries
and swap the one the helper binaries load. Either way the two configurations must differ in
the code under test and nothing else — same compiler, same flags, same helper binaries.

Point one capture at both:

```
Meta/style-update-capture.py capture paired.json --runs 5 \
    --config before=Build/before/bin/WebDriver --config after=Build/after/bin/WebDriver
Meta/style-update-capture.py compare paired.json paired.json --a before --b after
```

The capture alternates the two configurations run by run, so a machine that warms up or slows
down over the sitting affects both equally. The comparison then reports per-metric medians
and, because the runs are paired, the median of the per-run differences.

Read the counters before the clocks. A change meant to do the same work differently should
report the physical-work fields identical between the two configurations; the comparison
prints every counter whose value set differs. If one moved, that is the finding, and the
timings under it mean little until it is explained. If the counters match and the paired
median moves in the same direction across repetitions, the change moved time rather than
work.
