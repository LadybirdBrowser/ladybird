# Regression tests for output from our Qt AT-SPI2 bridge

These tests exercise Ladybird’s Qt AT-SPI2 bridge end-to-end. Each test launches a real Ladybird process on a fixture HTML page, connects to it as an AT-SPI2 client via Python `gi.repository.Atspi`, and asserts on what the bridge actually exposes.

## What’s here

- `harness/` — Python helpers: launch Ladybird with an isolated D-Bus/AT-SPI2 environment, walk the accessibility tree, query role/text/state/ attributes in a uniform way.
- `input/` — HTML fixtures. Each fixture is a small, self-contained page that exercises one or a handful of specific bridge behaviors.
- `tests/` — Python test files, grouped by behavior category. One assertion per invariant; names spell out the invariant being checked.
- `run_tests.sh` — local runner. Brings up Xvfb + a scratch D-Bus session + `at-spi-bus-launcher`, then drives the tests with `pytest`.

## Why this layer

We have tree-dump tests under `Tests/LibWeb/Accessibility/` that compare the pre-IPC accessibility data model against expectation files. Those catch regressions in role/name/ancestor relationships — anything observable in the serialized data. They do _not_ catch regressions in:

1. What the Qt AT-SPI2 bridge emits over D-Bus (role mapping, interface advertisement, text flattening, U+FFFC placement, per-character rects, visual-line boundaries).
2. When the bridge fires events (focus, state-changed).
3. Which accessibility actions route to which WebContent calls.
4. Multi-tab lifecycle (hide/show, interface deregistration).

Those are exactly the properties Orca relies on. Bugs there manifest as “the screen reader says the wrong thing/nothing at all” — impossible to spot in a tree dump. That’s what these tests are for.

## Running the tests locally

```
./Tests/LibWeb/AccessibilityBridge/run_tests.sh
```

Prerequisites (once per machine):

- `Xvfb` (a virtual X server).
- `at-spi2-core` and `at-spi2-atk` (for `at-spi-bus-launcher`,
  `at-spi2-registryd`, and `libatspi`).
- `python3-gi` (for `gi.repository.Atspi`).
- `pytest`.
- A Release build of Ladybird at `Build/release/bin/Ladybird`.

The runner starts a private X display + a fresh D-Bus session bus + the AT-SPI2 bus on top — then runs pytest inside that environment. No changes to the host session.

## Flakiness budget

Tests wait up to 15 seconds for Ladybird to start, and for its accessibility tree to become visible on the bus. Anything slower is flakiness and should be investigated — not patched around with longer timeouts.

## Adding a new test

1. Drop an HTML fixture in `input/` (keep it minimal — one behavior per page when possible; multi-behavior pages are fine when the behaviors naturally co-occur).
2. Add a test function in `tests/test_<category>.py` that uses the `ladybird` fixture, loads the page, and asserts on it.

See any existing test for the full shape.
