# Regression tests for output from our Qt AT-SPI2 bridge and from Orca

These tests exercise Ladybird’s Qt AT-SPI2 bridge and Orca integration end-to-end. Each test launches a real Ladybird process on a fixture HTML page, connects to it as an AT-SPI2 client via Python `gi.repository.Atspi`, and asserts on what the bridge actually exposes — and what Orca will actually say.

## What’s here

- `harness/`: Python helpers: launch Ladybird with an isolated D-Bus/AT-SPI2 environment, walk the accessibility tree, query role/text/state/attributes in a uniform way, and (Orca tests) load Orca + our Ladybird Orca script — so tests can drive Orca speech-generation directly.
- `input/`: HTML fixtures. Each fixture is a small, self-contained page that exercises one or a handful of specific bridge behaviors.
- `tests/`: Bridge tests. Python test files grouped by behavior category. Assert directly on the Qt bridge’s AT-SPI2 output for a given HTML page.
- `tests_orca/`: Orca tests. Import Orca and our Ladybird script, instantiate against the live Ladybird AT-SPI2 accessible, monkey-patch `speech.speak`, drive Orca commands (`present_object`, `get_sentence_contents_at_offset`, `find_all_with_role`, etc.), and assert on what Orca would have spoken.
- `run_tests.sh`: local runner. By default, runs both layers. Use `--layer=1` or `--layer=2` to limit. With `CI=1` (or `--isolated`), spins up Xvfb + a scratch D-Bus session + `at-spi-bus-launcher` first.

## Why these layers

We have tree-dump tests under `Tests/LibWeb/Accessibility/` that compare the pre-IPC accessibility data model against expectation files. Those catch regressions in role/name/ancestor relationships — anything observable in the serialized data. They do _not_ catch regressions in:

1. What the Qt AT-SPI2 bridge emits over D-Bus (role mapping, interface advertisement, text flattening, U+FFFC placement, per-character rects, visual-line boundaries).
2. When the bridge fires events (focus, state-changed).
3. Which accessibility actions route to which WebContent calls.
4. Multi-tab lifecycle (hide/show, interface deregistration).

Those are exactly the properties Orca relies on. Bugs there manifest as “the screen reader says the wrong thing/nothing at all” — impossible to spot in a tree dump. That’s what these tests are for.

And even when the bridge output is correct, Orca’s own code paths (plus the multiple monkey patches our Ladybird script installs) can still misbehave — and that’s where bugs like the I-key first-char-only regression manifest.

`tests/`: asserts on the bridge’s direct AT-SPI2 output.
`tests_orca/`: imports Orca into the test process, instantiates our Ladybird script, captures every `speech.speak` call, and asserts on what a screen-reader user would hear. Same Ladybird launcher backs both.

## Running the tests

The tests are wired into ctest alongside the regular LibWeb suite, so the recommended way to run them is:

```
ctest --test-dir Build/release -R AccessibilityBridge --output-on-failure
```

That runs both layers against your live AT-SPI2 session. The Orca tests skip cleanly if the `orca` Python package isn’t importable.

The runner is also usable directly:

```
./Tests/LibWeb/AccessibilityBridge/run_tests.sh            # both layers
./Tests/LibWeb/AccessibilityBridge/run_tests.sh --layer=1  # bridge tests only
./Tests/LibWeb/AccessibilityBridge/run_tests.sh --layer=2  # Orca tests only
```

The runner starts a private X display + a fresh D-Bus session bus + the AT-SPI2 bus on top — then runs pytest inside that environment. No changes to the host session.

Set `LADYBIRD_BINARY=/path/to/Ladybird` if your build isn’t at the default `Build/release/bin/Ladybird`.

Set `CI=1` (or pass `--isolated`) to make the runner spin up a private Xvfb display, a fresh D-Bus session, `at-spi-bus-launcher`, and `at-spi2-registryd` before the tests run — independent of your live session. GitHub Actions automatically sets `CI=true` — so our CI Lagom job takes the isolated path without any extra configuration. Use that locally only when your session’s AT-SPI2 state is wedged — or when you want to debug what CI is seeing.

Prerequisites:

- `python3-gi` and `at-spi2-core` are needed for either mode.
- Isolated mode additionally needs `xvfb`, `dbus` (for `dbus-run-session`), and the `at-spi-bus-launcher` + `at-spi2-registryd` binaries that ship with `at-spi2-core`.
- Layer 2 needs the distro’s `orca` package; without it, Layer 2 is skipped cleanly instead of failing.
- A Release build of Ladybird (by default at `Build/release/bin/Ladybird`; override with `LADYBIRD_BINARY`).

## Flakiness budget

Tests wait up to 15 seconds for Ladybird to start, and for its accessibility tree to become visible on the bus. Anything slower is flakiness and should be investigated — not patched around with longer timeouts.

## Adding a new test

1. Drop an HTML fixture in `input/` (keep it minimal — one behavior per page when possible; multi-behavior pages are fine when the behaviors naturally co-occur).
2. Add a `TestCase` subclass in `tests/test_<category>.py` (bridge tests) or `tests_orca/test_<category>.py` (Orca tests). Subclass `AccessibilityBridgeTestCase`, set `FIXTURE = "<file>.html"`, and add `test_*` methods.

For the full shape, see any existing test file.
