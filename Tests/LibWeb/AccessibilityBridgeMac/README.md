# Regression tests for the macOS NSAccessibility surface

These tests exercise Ladybird’s macOS accessibility implementation end-to-end. Each test launches a real Ladybird process on a fixture HTML page, connects to it as an NSAccessibility client via PyObjC + AXUIElement, and asserts on what the AX surface actually exposes — and what VoiceOver would see when walking that surface.

## What’s here

- `harness/`: Python helpers: launch Ladybird with a fixture, walk the NSAccessibility tree, and query role/value/title/attributes uniformly.
- `tests/`: Test files grouped by behavior category. Assert directly on the AX surface for a given fixture.
- `run_tests.sh`: local runner.

Fixtures are reused from the `Tests/LibWeb/AccessibilityBridge/input/` directory rather than duplicated. The fixtures are pure HTML — they exercise role/name/structure invariants that are equally meaningful on any platform.

## Why this exists

We have tree-dump tests under `Tests/LibWeb/Accessibility/` that compare the pre-IPC accessibility data model against expectation files. Those catch regressions in the data model — anything observable in the serialized `AccessibilityNodeData`. They do *not* catch regressions in:

1. What the macOS `LadybirdAccessibilityElement` wrapper exposes via NSAccessibility (role mapping, attribute advertisement, focus state, parameterized-attribute responses).
2. What `accessibilityFocusedUIElement` returns at runtime as state changes.
3. Whether parameterized attributes (text markers, search predicates) return advancing/stable values vs. always-the-same lies.

Those are exactly the properties VoiceOver relies on. Bugs there manifest as "VoiceOver re-reads the same paragraph forever / says the wrong thing / pauses unexpectedly" — which are impossible to spot in a tree dump. So, spotting regressions for those are what these tests are for.

## Layer 2 (VoiceOver speech capture)

Not implemented. VoiceOver is a closed binary; there is no public hook for “what was just synthesized”. In contrast, the corresponding Linux harness has a “Layer 2” because Orca is Python and we can monkey-patch `orca.speech.speak`. For macOS, the practical alternative is to walk the same NSAccessibility surface VoiceOver walks, applying the same role-to-utterance heuristics. That’s what these tests do — Layer 1 with VoiceOver-style traversal, not actual VoiceOver output.

## Running the tests

The tests are wired into ctest:

```
ctest --test-dir Build/release -R AccessibilityBridgeMac --output-on-failure
```

The runner is also usable directly:

```
./Tests/LibWeb/AccessibilityBridgeMac/run_tests.sh
```

Set `LADYBIRD_BINARY=/path/to/Ladybird.app/Contents/MacOS/Ladybird` if your build isn’t at the default `Build/release/bin/Ladybird.app/Contents/MacOS/Ladybird`.

## Prerequisites

- macOS (Darwin) with the AppKit port build (`-DLADYBIRD_GUI_FRAMEWORK=AppKit`, the default on macOS).
- A Release build of Ladybird (default at `Build/release/bin/Ladybird.app/...`; override with `LADYBIRD_BINARY`).
- PyObjC frameworks (`pyobjc-framework-ApplicationServices`, `pyobjc-framework-Cocoa`). macOS’s system Python at `/usr/bin/python3` ships with PyObjC preinstalled; **Homebrew’s `python3` typically does not**. The runner auto-detects a Python that has the modules, probing in this order: the `PYTHON` env var, then `python3` on PATH, then `/usr/bin/python3`, then `/Library/Developer/CommandLineTools/usr/bin/python3`. If your `python3` on PATH is Homebrew and lacks PyObjC, either set `PYTHON=/usr/bin/python3` or install PyObjC into your active Python: `python3 -m pip install --user pyobjc-framework-ApplicationServices pyobjc-framework-Cocoa`.
- **Accessibility permission** for the calling shell/IDE. Grant via System Settings → Privacy & Security → Accessibility. Without it, every AXUIElement read returns `kAXErrorAPIDisabled` — and the harness fails fast with a clear message.

## Flakiness budget

Tests wait up to 15 seconds for Ladybird to start and for its AXWebArea to populate (children present). Anything slower is flakiness and should be investigated — not patched around with longer timeouts.

## Adding a new test

1. Drop an HTML fixture in `Tests/LibWeb/AccessibilityBridge/input/` (the cross-platform fixtures directory). Keep it minimal — one behavior per page when possible.
2. Add a `TestCase` subclass in `tests/test_<category>.py`. Subclass `AccessibilityBridgeMacTestCase`, set `FIXTURE = "<file>.html"`, and add `test_*` methods.

For the full shape, see any existing test file.
