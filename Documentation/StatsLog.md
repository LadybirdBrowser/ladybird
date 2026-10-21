<!--
Copyright (c) 2026-present, the Ladybird developers.

SPDX-License-Identifier: BSD-2-Clause
-->

# Process statistics

Start Ladybird or WebDriver with `--stats-log-dir=/absolute/path` to record engine statistics. The
directory is created if necessary. Use a fresh directory and a separate browser profile for each
run. WebDriver forwards the option to the browser, which forwards it to its helper processes,
including helpers started later or restarted after a crash. A browser started with the option
never hands its URLs to a browser that is already running. Logging is disabled by default.

Each process appends UTF-8 JSON lines to `<directory>/<process>.<pid>.jsonl` and prints its file
path to stderr. Files are opened with mode 0600 and are closed on exec. Existing files are appended
to, never truncated. Opening failures fail startup. Writing failures print the path and error and
quit the process's event loop with exit code 1. A forcibly terminated process can leave an
incomplete final line.

Every record has these fields:

| Field | Meaning |
| --- | --- |
| `type` | `sample` or `event` |
| `time_ns` | Integer nanoseconds from `CLOCK_MONOTONIC`, shared across processes on a host |
| `process` | `Ladybird`, `WebDriver`, `WebContent`, `WebWorker`, `RequestServer`, `Compositor`, `ImageDecoder`, `AudioServer`, `MediaServer`, or `VideoDecoderServer` |
| `pid` | Integer operating system process ID |

For example:

```json
{"type":"sample","time_ns":1234567890,"process":"WebContent","pid":123,"values":{"gc.heap_blocks":42}}
{"type":"event","time_ns":1234567891,"process":"WebContent","pid":123,"name":"load","data":{"page_id":1,"navigable_id":"1:1","document_id":"1:1","url":"https://example.com/"}}
```

Samples have a `values` object containing only numbers with dotted keys. Each process takes an
initial sample after initialization and then samples every 100 ms on its main event loop. Busy
processes can delay samples. The logger neither interrupts JavaScript nor reads GC or graphics state
from another thread. Add process-specific counters in the callback passed to
`Core::StatsLog::start_sampling()` using `values.set("category.name"sv, value)`.

## Sample values

All sizes are bytes. Counts are integers. Duration units are given by their key suffixes.
Unsupported values are omitted, rather than reported as zero.

| Key | Source |
| --- | --- |
| `mimalloc.current_rss_bytes` | `mi_process_info()` current RSS estimate. On Linux mimalloc 2.2 uses its committed-byte count as the estimate, not actual process RSS. |
| `mimalloc.peak_rss_bytes` | `mi_process_info()` peak process RSS. On Linux this is `getrusage(RUSAGE_SELF).ru_maxrss`, converted to bytes. |
| `mimalloc.current_commit_bytes` | `mi_process_info()` current commit result |
| `mimalloc.peak_commit_bytes` | `mi_process_info()` peak commit result |
| `malloc.arena_bytes` | glibc `mallinfo2().arena`, memory obtained for system malloc arenas |
| `malloc.allocated_bytes` | glibc `mallinfo2().uordblks`, allocated arena bytes |
| `malloc.free_bytes` | glibc `mallinfo2().fordblks`, free arena bytes |
| `malloc.mmap_bytes` | glibc `mallinfo2().hblkhd`, memory obtained for system malloc mmap allocations |
| `gc.allocated_bytes_total` | Heap's existing cumulative allocation counter, including external allocation notifications and storage cells |
| `gc.allocated_bytes_since_gc` | Heap's existing allocation threshold counter, adjusted by external frees |
| `gc.heap_blocks` | Number of currently owned GC heap blocks |
| `gc.heap_block_bytes` | `gc.heap_blocks` times the heap block size, including block headers and unused cell slots |
| `gc.cell_allocators` | Number of registered type-isolating cell allocators in this heap |
| `gc.threshold_bytes` | Current GC allocation threshold |
| `gc.live_cell_bytes` | Cell slots surviving the most recently completed sweep |
| `gc.live_external_bytes` | External memory reported by those surviving cells |
| `gc.collections` | Number of collections started since logging was enabled |
| `gc.completed_collections` | Number of collections whose sweeps have finished |
| `gc.total_duration_ns` | Sum of active GC durations for completed collections |
| `gc.last_duration_ns` | Active duration of the last completed collection |
| `gpu.skia_resource_cache_bytes` | Compositor's main Skia GPU context resource cache usage |
| `requests.clients` | RequestServer's data client connection map size, excluding the browser control connection |
| `requests.transfer_leases` | RequestServer's outstanding request transfer lease map size |
| `dom.documents` | WebContent main event loop's registered document count, including detached and decoded SVG documents pending collection |
| `images.available_entries` | Sum of registered documents' lists of available images map sizes |
| `images.shared_requests` | Sum of registered documents' shared image request map sizes, including pending requests |
| `css.document_style_sheet_entries` | Sum of document style scopes' sheet vector sizes, excluding shadow scopes and adopted sheets |
| `fonts.face_entries` | Sum of registered documents' `FontFaceSet` sizes, including loading faces |
| `fonts.system_fallback_cache_entries` | Process-wide system fallback font cache map size, including cached misses, read under its mutex |
| `rendering.updates` | Main event loop's rendering updates run counter |
| `rendering.paints` | Main event loop's paint counter |
| `rendering.update_duration_us` | Main event loop's rendering update time counter, in microseconds |

GC values belong to the main VM heap in WebContent and WebWorker. Live sizes and completed durations
start at zero until the first sweep finishes. Heap block bytes exclude the reserved address range,
unused type-isolated chunks, weak blocks, and blocks retained for reuse or awaiting background
decommit. They are not a measurement of resident memory or committed memory. No heap walk is
performed for a sample.

AK uses mimalloc on supported platforms except address and thread sanitizer builds, where its sample
keys are omitted. System malloc values are present on glibc 2.33 or newer and cover allocations
served by glibc, not allocations served by mimalloc or the GC block allocator. The Skia key is
omitted with CPU painting. Skia cache usage does not include every GPU allocation, driver
allocation, or backing store.

WebContent structural samples read existing container sizes in O(1) per registered document. They
enumerate the document registry without sorting it, walking trees, creating wrappers, or forcing a
render-thread read. Entries are counted per document and can refer to the same underlying image,
sheet, or font. Counts include detached documents until collection. The rendering counters are the
existing diagnostic counters and can be reset by the test-only internals API. DOM node and realm
counts have no existing constant-time total. Layout and paintable state belongs to the render
thread; its diagnostic accessor forces a synchronous read. Decoded image bytes require visiting
image data and SVG caches. These values are deliberately omitted.

## Events

Events have a `name` string and a `data` object. Navigation and document lifecycle events cover
top-level traversables. IDs are scoped to a browser instance. Cross-process IDs are strings of the
form `namespace:local`, preserving their exact values in floating-point JSON consumers.

| Name | Process | Data |
| --- | --- | --- |
| `navigation_start` | Ladybird | `navigable_id` (cross-process ID string), `navigation_id` (string), `url` (string) |
| `DOMContentLoaded` | WebContent | `page_id` (integer), `navigable_id` (cross-process ID string), `document_id` (cross-process ID string), `url` (string) |
| `load` | WebContent | Same fields as `DOMContentLoaded` |
| `frame` | Compositor | `page_id` (integer), `changed` (boolean), `damage_x`, `damage_y`, `damage_width`, `damage_height` (integer device pixels) |
| `rendering_pending` | WebContent | `page_id` (integer), `navigable_id` (cross-process ID string), `pending` (boolean), `reasons` (object of integer counts) |
| `gc` | WebContent or WebWorker | `collection`, `duration_ns`, `pause_duration_ns`, `sweep_duration_ns`, `bytes_before`, `bytes_after`, `live_cell_bytes`, `live_external_bytes` (integers) |

Navigation starts are recorded when the browser admits a navigation. Lifecycle markers are recorded
immediately before DOM event dispatch, excluding event handlers. Match them by `navigable_id`.
A navigation can move to a new WebContent process and page ID. Same-document navigations can have a
start without new lifecycle events. Canceled navigations may never produce lifecycle events.
Detached documents and decoded SVG image documents do not produce lifecycle markers.

GC events are written after sweeping finishes. Durations measure active collection work, excluding
idle time between incremental sweep slices. `pause_duration_ns` includes marking, finalization and
synchronous sweep or incremental-sweep setup.
`sweep_duration_ns` sums incremental block sweeps; `duration_ns` is their sum. `bytes_before` and
`bytes_after` count owned heap block bytes, including unused slots. They are not process RSS.

The compositor records a frame immediately before publishing its rendered surface to the browser.
GPU work has completed, or on Metal has been scheduled for the receiving surface to wait on.
This is a rendered-frame timestamp, not a hardware scanout timestamp. Child surfaces and screenshots
do not produce separate frame events. Match frames to lifecycle markers by `page_id`.

`changed` is the existing damage rectangle's non-emptiness. Damage is conservative: repainting can
produce identical colors. No readback or pixel hashing is performed. Fully suppressed frames do not
produce events. The last changed frame after a bounded quiet period gives a conservative estimate of
when the page finished painting. Animation and media can keep producing changed frames indefinitely.

## Pending rendering work

`rendering_pending` reports the page's active documents hosted in this WebContent process. Changes
coalesce across an HTML task and its microtasks, or at the Core event-loop boundary for other engine
callbacks. The first snapshot establishes state; subsequent snapshots are written only when totals
change. Replace snapshots by `(pid, page_id)` and combine live pages sharing `navigable_id` for
isolated frames. Absence of a snapshot is unknown, not idle.

Detached, destroyed and decoded SVG image documents are excluded. Hidden documents and frames with
no displayed viewport are excluded from every counter. Display eligibility uses the existing
container viewport report, visibility state and local-root viewport intersection. This excludes
zero-size, offscreen and undisplayed frames; it does not test pixel occlusion by other elements.
A newly attached child contributes after its first displayed viewport report. A stats-only display
eligibility notification is routed through the browser to isolated hosts, including descendants when
an embedding changes. It carries no presentation identities and does not change viewport geometry,
web visibility or rendering decisions.

`pending` is true when any of these `reasons` is nonzero. All keys are always present:

| Reason | Count |
| --- | --- |
| `needs_rendering_update` | Documents with an invalidation not consumed by a presentation commit or a no-op rendering update |
| `image_decodes` | Initial shared image decodes and window `createImageBitmap(Blob)` operations awaiting completion, failure or stop |
| `font_loads` | Entries in document `FontFaceSet` loading lists |
| `style_sheet_loads` | Pending stylesheet-request delayers plus pending CSS import rules |
| `resource_fetches_in_flight` | Window-client fetches through final response source completion, failure or cancellation |
| `scheduled_animation_frame_callbacks` | Window callbacks awaiting the next animation-frame callback batch |
| `load_event_delayers` | Existing document load-event delayers |
| `document_loads` | Documents whose readiness is not yet `complete` |

Counters describe engine registrations and overlap intentionally. Fetch completion observes the final
response's original body source, so unread queued bytes do not keep a completed transfer pending.
Operations are keyed by their GC cell and hold a weak document reference. Completion, cancellation
and finalization remove registrations without retaining resources or documents. Changes take O(1)
bookkeeping; snapshots aggregate the attached documents without walking DOM or layout trees.

`pending: false` covers tracked work at that boundary. It does not mean a committed recording has
reached the browser. Combine it with lifecycle and frame events and a bounded quiet period. It does
not cover future timers, input, worker activity, animation or video frames. An ongoing animation-frame
callback loop remains pending. These signals do not promise permanent visual quiescence.

## Sandboxing and overhead

Each helper opens its own file before installing its sandbox. Linux retains the open descriptor;
macOS grants write access to that exact file. Sandboxes remain enabled. The directory must be
absolute and files must be regular files. Symlinks are rejected. The macOS ProcessReaper is a system-library-only watchdog without an engine
event loop and does not participate in stats logging.

Logging disabled leaves no logger, sampling timer or pending-work state. Hooks check a
constant-initialized thread-local pointer before collecting statistics. Logging enabled adds JSON
serialization and synchronous file writes, so use a local filesystem for performance investigation.
