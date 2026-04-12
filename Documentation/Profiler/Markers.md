# Profiler markers

Profiler markers are timestamped events emitted by browser and engine code so a profiler UI can show what happened on the timeline: layout, style, script parsing, IPC, network, media decode, GC, and similar phases.

The marker system is designed with the Gecko Profile Format in mind because the first exporter targets profiler.firefox.com. The collected marker stream is format-neutral though — a different exporter can map the same data to Perfetto, Chrome trace-event JSON, or a Ladybird-specific UI.

## Public API

Include `<LibCore/Markers.h>` at marker emit sites. The API is:

| Macro | Use |
|-------|-----|
| `MARKER_INSTANT(name, type, category, fields)` | Point-in-time event |
| `MARKER_START_TIME(var)` | Capture a start timestamp (empty when disabled) |
| `MARKER_INTERVAL(name, type, category, start, fields)` | Interval from a captured start to now |
| `MARKER_INTERVAL_START(name, type, category, fields)` | Open-ended interval start |
| `MARKER_INTERVAL_END(name, type, category, fields)` | Close a paired interval |
| `MARKER_SCOPE(name, type, category)` | RAII interval for the enclosing scope |
| `MARKER_SCOPE_FIELDS(name, type, category, fields)` | RAII interval with lazily evaluated fields |

All macros gate on `g_marker_collector_active` before evaluating arguments. When no collector is active, the cost is one relaxed atomic load and a predicted-not-taken branch.

Supporting types in `Markers.h`:

- `MarkerCategory` — broad category enum (Layout, JavaScript, GC, etc.)
- `MarkerPhase` — instant, interval, interval-start, interval-end
- `MarkerField` — key-value pair for marker payload data
- `MarkerFieldValue` — `Variant<StringView, String, double, i64, size_t, bool>`
- `MarkerString` — `Variant<StringView, String>` for marker names
- `MarkerScope` — RAII class behind the `MARKER_SCOPE*` macros

All macros route through a single implementation function:

```cpp
void marker_add(MarkerPhase, MarkerString name, StringView type,
    MarkerCategory, Optional<MonotonicTime> start, Vector<MarkerField, 4>);
```

When `start` is empty, `marker_add` uses the current time. For intervals, `start` is the captured beginning and `marker_add` captures the end.

Do not include `<LibCore/MarkerCollector.h>` from instrumentation sites. That header is for code that owns or reads the collector.

## Header boundary

`Markers.h` is included by many libraries. Keep it stable and thin. It does not expose:

- `MarkerCollector` class or `Marker` struct
- Mutex, `AK::Function`, or thread registry dependencies
- Gecko exporter details

This lets collector and exporter internals change without recompiling marker call sites.

The active flag (`g_marker_collector_active`) is a relaxed atomic boolean, matching Firefox's `profiler_is_active()` pattern. Firefox reads a relaxed atomic bitfield; we use a single bool since we only need one gate.

## Why macros

C++ evaluates function arguments before entering the function body. Macros put the active check around argument evaluation so disabled instrumentation does not format strings, serialize URLs, build field vectors, or read clocks:

```cpp
MARKER_INSTANT("Fetch start"sv, "Text"sv, Core::MarkerCategory::Network,
    { { "name"sv, request.url().to_string() } });
```

When no collector is active, `url().to_string()` is not called. `MARKER_SCOPE_FIELDS` wraps its fields in a lambda that `MarkerScope` only invokes after the active check:

```cpp
MARKER_SCOPE_FIELDS("Layout"sv, "Layout"sv, Core::MarkerCategory::Layout,
    {
        { "viewportWidth"sv, static_cast<double>(viewport_rect().width().to_double()) },
    });
```

For conditional fields, construct the `MarkerScope` directly with a lambda:

```cpp
Core::MarkerScope marker { "DOMEvent"sv, "DOMEvent"sv, Core::MarkerCategory::DOM,
    [&]() -> Vector<Core::MarkerField, 4> {
        Vector<Core::MarkerField, 4> fields;
        fields.append({ "eventType"sv, event.type().bytes_as_string_view() });
        if (is<Node>(*target))
            fields.append({ "target"sv, as<Node>(*target).node_name().to_string() });
        return fields;
    } };
```

## Data flow

`MarkerCollector`'s constructor sets `g_marker_collector_active = true` and `g_marker_collector = this`. The destructor clears both. Macros check the atomic bool; `marker_add` fetches the internal pointer and appends a `Marker` under a mutex.

Readers use `take_markers_snapshot()` for thread-safe copies (e.g., `internals.collectMarkers()`).

## Categories

`MarkerCategory` is a stable enum shared by markers and profiler labels. Keep categories broad — put specificity in marker names and fields, not new categories.

## Disabled-path rules

- Put expensive values inside macro arguments or `MARKER_SCOPE_FIELDS` lambdas.
- Do not compute marker-only data before the macro gate.
- Use `MARKER_START_TIME` instead of `MonotonicTime::now()` for marker-only timestamps.
- Do not call `marker_add` directly from call sites — use the macros.

Good:

```cpp
MARKER_SCOPE_FIELDS("Parse CSS"sv, "ParseCSS"sv, Core::MarkerCategory::Parser,
    { { "url"sv, source_url.serialize() } });
```

Bad — serializes unconditionally:

```cpp
auto url = source_url.serialize();
MARKER_SCOPE_FIELDS("Parse CSS"sv, "ParseCSS"sv, Core::MarkerCategory::Parser,
    { { "url"sv, url } });
```
