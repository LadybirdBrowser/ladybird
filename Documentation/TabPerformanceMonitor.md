<!-- Copyright (c) 2026-present, the Ladybird developers. -->
<!-- SPDX-License-Identifier: BSD-2-Clause -->

# Tab performance monitor

Enable **Show tab performance monitor** in **Settings → Advanced**, or set
`debug.ui.show_tab_performance_monitor` to `true`. It defaults to `false`.
The Qt and AppKit toolbars display the current tab's statistics inline.
Each tab retains its own CPU history while monitoring is enabled.

`LibWebView::TabPerformanceMonitor` resolves browser ownership and supplies
`TabPerformanceStats` to the frontend. `TabPerformanceAccumulator` performs
counter differencing and windowing independently of the UI and OS APIs.

## Metric definitions

* **CPU:** CPU-time deltas divided by actual monotonic elapsed time. 100% is
  one logical CPU; values above 100% are expected. All live and terminated
  threads in exclusively owned WebContent and WebWorker processes contribute.
  Registered page and embedded-frame ownership identify WebContent owners.
  Worker ownership is resolved transitively, including nested workers and
  shared workers whose owners all resolve to the same tab. Processes shared
  across tabs, unresolved owners and cyclic ownership remain unattributed.
  The first sample establishes a baseline. The sparkline retains ten seconds.
* **MEM:** An estimate of owned process memory, counted once per process.
  macOS uses the kernel's physical-footprint ledger, including compressed
  memory, rather than RSS. Linux uses resident minus shared pages from
  `/proc/PID/statm`, a cheap private-resident approximation that omits
  file-backed private pages and swapped-out memory. These platform estimates
  are not directly comparable. Other platforms report unavailable until a
  collector is implemented. Shared helper/cache allocations and resources
  without a process-level ownership charge are excluded.
* **Download/upload:** HTTP response/request body bytes transferred by curl,
  before response decompression. Disk-cache hits contribute no traffic.
  Headers, TLS/TCP overhead, WebSockets, and helper-initiated background
  requests without an originating page or exclusive worker are excluded.
  A request keeps its originating process/page context when transferred to
  another client. The browser resolves that context to a tab; traffic whose
  original page or worker has already disappeared is left unattributed.
  Byte units are decimal. Rates use a rolling second, with delayed batches
  weighted by their actual sampling interval.
* **FPS:** Accepted compositor backing-store swaps over the last second,
  including compositor-driven animation. This measures frames delivered to
  the frontend, not hardware display scanout or `requestAnimationFrame` calls.
  After a second without a frame the value is `FPS —`. FPS uses neutral text.

CPU, memory and FPS values use neutral text. Labels use 65% opacity and
the sparkline uses a subdued stroke. Network download is blue and upload
orange. Fixed-width columns and tabular digits prevent layout shifts.

## Sampling and overhead

The browser samples owned process counters every 500 ms. RequestServer runs
its own 500 ms timer and **pushes** byte deltas asynchronously to the browser;
there is no UI polling or synchronous traffic-statistics IPC. Completed
requests contribute their final delta before their curl handle is released.
Batches coalesce requests with the same originating process/page.

Disabling the setting destroys both timers, clears histories and pending byte
batches, and disconnects the browser's traffic callback. RequestServer does
not sample curl counters or push traffic messages while disabled. Existing
request setup retains small ownership metadata, and the frame delivery path
has a disabled-state check. Re-enabling establishes fresh CPU and in-flight
request baselines; restarting RequestServer restores an enabled subscription.

Shared helper CPU is currently excluded: the network, image decoder, compiler
and compositor processes do not yet expose per-job CPU accounting. Their
whole-process CPU is never assigned to a tab. Additional explicitly owned
memory or helper CPU can be added to the accounting layer without putting
platform or ownership queries in toolbar widgets.
