# RequestServer wire-activity logging

RequestServer emits up to four `dbgln` lines per HTTP fetch that actually
hit the network, so you can correlate "dead air" in a profile with the
download (or stall) responsible for it.

Cache hits do **not** appear here — only requests that produced traffic.
That is the point: if the log is silent during a profile gap, the gap is
not a download.

## The lines

For every fetched request you get a `wire:` line. Successful fetches that
delivered any body also get `wire+:` (decoded view), `wire++:` (encoded
view, when curl provided wire-level progress), and `wire^:` (pre-network
time spent inside RequestServer plus drain delay). When more than one
request completes in the same curl multi tick, a single `wire-batch:`
line is emitted explaining why their `wire:` timestamps cluster.

Several diagnostic lines fire only when something interesting happened
and don't belong to any single request: `wire-stall:` flags
event-loop blocks and slow synchronous curl calls, `wire-burst:` flags
request floods from a client, `wire-pipe-pressure:` flags WebContent
back-pressure on the response pipe, `LibDNS wire-dns:` flags slow DNS
lookups (especially the blocking `getaddrinfo` fall-back), and `UI
wire-cookie:` flags slow cookie-jar lookups on the UI side. See the
"Process-wide diagnostic lines" section below.

### `wire:` — one-line summary

```
wire: KIND METHOD URL HTTP-VERSION -> STATUS
    | wire X.X KiB sent X.X KiB
    | total N ms = queue N + dns N + tcp N + tls N + req N + wait N + body N
    | wire X.X KiB/s avg, X.X KiB/s during body
```

Fields:

- **KIND** — what happened:
  - `DOWNLOAD` — full body fetched from origin.
  - `REVAL-304` — conditional GET hit the cache; server returned 304, we
    served the cached body.
  - `REVAL-FULL` — conditional GET that the server invalidated (returned
    new content), so we replaced the cache entry.
  - `FAIL` — curl failed before completion. The line collapses to
    `... -> error: <curl message> (after N ms, wire X.X KiB)`.
  - `[bg]` suffix on the kind marks a background revalidation triggered
    by `stale-while-revalidate`.
- **wire X.X KiB** — bytes received **on the wire** (encoded; gzip /
  brotli / etc. as transferred). Comes from `CURLINFO_SIZE_DOWNLOAD_T`.
  Read this carefully — see the wire-vs-decoded section below.
- **sent X.X KiB** — request body bytes uploaded.
- **total N ms** — request lifetime, broken into libcurl phases. All
  phase numbers are wall-time deltas:
  - `queue` — time waiting in libcurl's pre-start queue (HTTP/2 and
    HTTP/3 stream multiplexing usually puts you here briefly).
  - `dns` — name resolution.
  - `tcp` — TCP handshake (or QUIC connect on HTTP/3).
  - `tls` — TLS handshake. Zero on plaintext, and zero on a reused
    connection — see "Reused-connection timing" below.
  - `req` — time between handshake completing and being ready to send
    the request bytes.
  - `wait` — server processing / time-to-first-byte. A high `wait` with
    everything else low means the origin took its time before sending
    the first response byte.
  - `body` — time from first response byte to last response byte.
- **wire X.X KiB/s avg** — `CURLINFO_SPEED_DOWNLOAD_T`, averaged over
  the entire request including TTFB. Useful as a sanity check, not as a
  throughput number.
- **wire X.X KiB/s during body** — wire bytes divided by `body` time.
  This is the throughput you actually got while bytes were flowing.

### `wire+:` — decoded-side per-chunk stats

```
wire+: decoded chunks=N bytes=X.X KiB span=N ms thru=X.X KiB/s
     | gap avg=N ms max=N ms stalls(>100ms)=N
     | worst gap began at +N ms after X.X KiB decoded
     | chunk bytes avg=N min=N max=N
```

Sampled inside our `WRITEFUNCTION` callback, so all numbers here are
**post-decompression** — i.e. the bytes our consumer actually receives.

- **chunks=N** — number of times curl handed us a buffer of decoded
  bytes.
- **bytes=X.X KiB** — total decoded body size (sum of all chunks).
- **span=N ms** — wall time between the first decoded chunk and the
  last.
- **thru=X.X KiB/s** — `bytes / span`, throughput as the consumer
  experienced it.
- **gap avg / max** — inter-chunk spacing. `max` is the longest pause
  between two consecutive decoded chunks.
- **stalls(>100ms)=N** — count of inter-chunk gaps longer than 100 ms.
  Useful for "was this one big pause, or several smaller ones?".
- **worst gap began at +N ms after X.X KiB decoded** — locates the
  worst gap in the timeline. If the answer is `+5 ms after 0.0 KiB`,
  the server flushed once and then thought; if it is
  `+800 ms after 90% of bytes`, the tail dragged.
- **chunk bytes avg / min / max** — distribution of decoded chunk
  sizes.

### `wire++:` — wire-side per-progress-tick stats

```
wire++: wire bytes=X.X KiB span=N ms thru=X.X KiB/s
      | wire max gap=N ms stalls(>100ms)=N
      | worst wire gap began at +N ms after X.X KiB on the wire
      | compression=Yx
```

Sampled inside `CURLOPT_XFERINFOFUNCTION`, which curl invokes both as
bytes hit the socket and as a heartbeat during silence. All numbers
here are **wire bytes** (encoded, before decompression).

- **wire bytes / span / thru** — encoded-side equivalents of the
  decoded line. `thru` is the real network throughput while bytes were
  flowing.
- **wire max gap / stalls / worst gap began at** — same idea as the
  decoded line, but reflecting actual silence on the network. This is
  what you want when the question is "is the server pausing or is curl
  buffering?".
- **compression=Yx** — `decoded_bytes / wire_bytes`. Common HTML pages
  land at 5–10×; static assets that are already compressed (.jpg,
  .mp3, .woff2) land at 1×.

### `wire^:` — pre-network time spent inside RequestServer

```
wire^: internal pre-curl=N ms = cache+init N + our-dns N + cookie N + curl-setup N | drain delay N ms
```

Wall-time accounting for everything that happens **before** libcurl owns the
request, plus the gap between curl marking it done and us logging.

- **internal pre-curl** — `curl_added_at - created_at`. Total time this
  request spent inside our state machine before `curl_multi_add_handle`.
  When this is a meaningful fraction of `total` on the `wire:` line, the
  delay is ours, not the network's.
- **cache+init** — Init / ReadCache / WaitForCache time. Big numbers
  here mean cache contention or a slow disk-cache lookup.
- **our-dns** — time inside our DNS resolver. Distinct from libcurl's
  `dns` field on the `wire:` line, which is **0** because we
  pre-resolve and pass the address via `CURLOPT_RESOLVE`. If the
  `wire:` line says `dns 0` and you trust the network is fine but
  things still feel slow, look here.
- **cookie** — round-trip IPC to the UI process to retrieve cookies for
  the URL. Skipped (`-`) for credential-less requests.
- **curl-setup** — gap from the last completed pre-network step to
  `curl_multi_add_handle`. Should be very small; if it isn't, something
  in `handle_fetch_state` got expensive.
- **drain delay** — `complete_observed_at - last_wire_byte_at`. How
  long between the last byte landing and `check_active_requests`
  noticing the completion. Usually sub-ms — non-zero means an event-loop
  delay between curl seeing the end and us draining its message queue.

Fields can show `-` when the relevant phase didn't run (e.g.
`our-dns -` for connect-only requests, `cookie -` when credentials
were disabled).

### `wire-batch:` — multiple completions per drain pass

```
RequestServer wire-batch: drained N completions in one curl multi tick
```

Emitted once per `check_active_requests` call when more than one request
completed since the previous drain. When you see this followed by N
`wire:` lines all sharing the same timestamp prefix, the clustering is
**cosmetic**: we drained the curl multi handle's done-queue in a tight
loop, so the `dbgln` calls happen microseconds apart. The actual
network completion times can be reconstructed from the `total` /
`body` numbers on each request, walked back from the drain timestamp.

If you want true per-request completion times, take the drain
timestamp and subtract each request's `drain delay` from `wire^:`.

## Process-wide diagnostic lines

These do **not** belong to any one request. They surface conditions that
affect the whole RequestServer process or its peers (WebContent, the UI
process). When investigating a stall, look at these first — a single
`wire-stall:` line often explains a dozen confusing `wire:` lines that
follow.

### `wire-stall:` — event-loop block detector

```
RequestServer wire-stall: N ms event-loop gap before 'LABEL' (previous handler: 'PREV')
RequestServer wire-stall: curl call 'LABEL' took N ms (synchronous in event loop)
```

Two flavours:

- **Event-loop gap.** Every notifier/handler we control samples
  `MonotonicTime::now()` on entry. If more than 100 ms passed since the
  previous sample, this fires, naming both the handler that just woke up
  and the previous handler that was the last thing to run. If `PREV` is
  the same handler each time, that handler is doing too much work
  synchronously. If `LABEL` is `curl-socket-ready` the kernel has been
  trying to deliver bytes to us for that long.

  **Exception:** when `LABEL` is `curl-timer-fired` and the timer fired
  within ±50 ms of the time libcurl asked us to wake it, the gap is
  by design (libcurl's internal heartbeat — typically 250 ms intervals)
  and the log line is suppressed. A real stall — the timer fires much
  later than its scheduled time — still reports.
- **Synchronous curl call.** `curl_multi_socket_action` should be very
  fast — it just hands a socket event to libcurl. If a single call
  exceeds 50 ms, libcurl is doing real work synchronously (TLS handshake
  on a fresh connection, certificate validation, callback dispatch into
  our `WRITEFUNCTION`). That work blocks every other request.

Labels currently emitted: `curl-timer-fired`, `curl-socket-ready`,
`check-active-requests`, `ipc-start-request`, `ipc-start-revalidation`,
`ipc-retrieved-cookie`, plus the curl wrappers
`multi_socket_action(timeout)` and `multi_socket_action(socket)`.

### `wire-burst:` — request flood from a client

```
RequestServer wire-burst: client N sent M requests in <100 ms
```

Per-client `start_request` arrival counter. When a single client lands
more than 5 `start_request` IPCs in a 100 ms window, the count is logged
the first time the next request arrives outside that window. Useful for
correlating a `wire-stall:` with "this is the burst that hit us".

### `wire-pipe-pressure:` — WebContent back-pressure on the response pipe

```
RequestServer wire-pipe-pressure: GET URL pipe full, buffering=N bytes
RequestServer wire-pipe-pressure: GET URL unblocked after N ms (peak buffered=N bytes); WebContent likely behind
```

Fires when `m_client_request_pipe->write` returns `EAGAIN` /
`EWOULDBLOCK`. RequestServer is producing response bytes faster than
WebContent's main thread can drain them — almost always because
WebContent is busy parsing HTML, executing JavaScript, or doing layout.
The "unblocked" line fires when the pipe drains and we resume writing,
but only if the back-pressure window lasted more than 50 ms. The
per-request totals are also surfaced in the `wire^:` line as
`pipe back-pressure events=N total=N ms peak-buffered=N bytes`. This is
the "WebContent is behind" diagnosis — it doesn't mean RequestServer is
slow.

### `LibDNS wire-dns:` — DNS lookup synchronous portion (and worker completions)

```
LibDNS wire-dns: lookup(NAME) path=PATH sync=N ms
LibDNS wire-dns: lookup(NAME) path=system-resolver-bg total=N ms = A(queue N + work N) | AAAA(queue N + work N) (off event loop)
```

The first form is emitted by `DNS::Resolver::lookup` whenever its
**synchronous** portion takes more than 5 ms. The synchronous portion is
what holds the event loop — anything past trivial here is interesting.
`path` classifies how the lookup was satisfied:

- `cache-hit` — answered entirely from our in-memory cache.
- `literal-ipv4` / `literal-ipv6` — `name` was an IP address literal.
- `system-resolver-bg` — dispatched two parallel `getaddrinfo` calls
  to ThreadPool workers, one for `AF_INET` (A records) and one for
  `AF_INET6` (AAAA records). The split avoids buggy stub resolvers
  (notably systemd-resolved under load) that drop the AAAA half of a
  coupled query and stall both. The promise resolves as soon as one
  side returns records, with a 50 ms grace window for the other side
  (Happy Eyeballs v2's "Resolution Delay", RFC 8305) so curl can prefer
  IPv6 when both are available.

  The synchronous portion is just the dispatch (sub-ms). When both
  workers have completed, a single line fires from the originating
  event loop with both halves of the breakdown:
    - `A(queue N + work N)` — IPv4 worker timing.
        - `queue` — wall time waiting in the ThreadPool work queue.
        - `work` — wall time inside `getaddrinfo` for AF_INET.
    - `AAAA(queue N + work N)` — same for the IPv6 worker.
    - `total` — wall-clock from dispatch to both workers reporting
      completion. May be much longer than what the user actually
      waited on if the slower side completed after the promise had
      already resolved.

  Either side reporting `-` for its fields would mean its results
  haven't been merged yet (only matters if you're reading partial state
  in a debugger; the log only fires after both completed). A large
  asymmetry between A and AAAA `work` (e.g., A=20 ms, AAAA=10000 ms)
  is a textbook stub-resolver-drops-AAAA pattern, but it no longer
  matters for the user's wait time — we resolved the promise off the
  fast side.
- `system-resolver-join-pending` — a worker is already running
  `getaddrinfo` for this name; we attached to its promise instead of
  spawning a second worker.
- `async-query` — sent a query over our DNS socket.
- `join-pending` — joined an in-flight async DNS query for the same name.
- `repeat-timeout` / `no-conn-dnssec-rejected` — error paths.

A consistent ~30 ms floor on `cache-hit` would indicate the cache is
not being short-circuited and we are paying lookup overhead per request.

A `system-resolver-bg completed in N ms (worker)` line with a large `N`
no longer freezes the event loop — it just means that one cold lookup
took a long time. Other requests should keep flowing during it.

### `UI wire-cookie:` — cookie-jar lookup time on the UI side

```
UI wire-cookie: get_cookie(URL) took N ms (N bytes returned)
```

Emitted by the UI process when `CookieJar::get_cookie` takes more than
5 ms. Pair this with the `cookie` field in `wire^:`:

- High `wire^:` cookie + matching `UI wire-cookie:` ⇒ the jar itself is
  slow.
- High `wire^:` cookie + no `UI wire-cookie:` ⇒ the UI process IPC
  was scheduled late (UI process busy with something else), but the
  jar lookup itself was fast.

## Wire vs decoded — the foot-gun

`wire X.X KiB` on the first line and the byte counts on the `wire+:`
line measure **different things**:

- **wire bytes** = encoded body on the network. This is what dictates
  network time.
- **decoded bytes** = bytes our consumer (the HTML parser, image
  decoder, etc.) sees. This is what dictates CPU work after download.

Comparing them gives you the compression ratio. Don't divide one by the
other's time and call it "throughput" — that's how you accidentally
report a network at 1.3 MB/s when the wire is doing 130 KB/s.

## Reused-connection timing

libcurl's phase markers (`namelookup`, `connect`, `appconnect`) report
**0** on a reused connection because no DNS / TCP / TLS work happened
again. The logger clamps each marker to the previous one's value so a
reused connection shows `dns 0 + tcp 0 + tls 0` and folds the queue
time into `queue` only. If you see a `req` value larger than zero on a
reused HTTP/2 / HTTP/3 connection, that is the time spent acquiring a
stream slot and writing the request, not redundant queue counting.

## Patterns and what they mean

| Symptom | Likely cause |
|---|---|
| One large `wire max gap` mid-transfer, low stall count, fast bytes either side (`wire++:`) | Server-side streaming / SSR with early flush. Origin sent the head, paused to generate the rest, then dumped it. Network is fine. |
| Many small stalls, low average throughput, no big gap (`wire++:`) | Bandwidth- or congestion-limited link, or HTTP/3 / TLS stack pacing. |
| Long single `wait` before any body, then everything fast (`wire:`) | Slow origin (server took its time computing the response). Not a network issue. |
| `wire thru` close to your link bandwidth, no stalls (`wire++:`) | You're saturating the pipe. Nothing to fix. |
| High `tls` on first request, `tls 0` afterwards (`wire:`) | Cold connection setup. Expected on first hit to a host. |
| Large `our-dns` with `dns 0` on `wire:` (`wire^:`) | Our DNS resolver is the bottleneck — libcurl reports 0 because we pre-resolved. Look at the resolver, not the network. |
| Large `cookie` (`wire^:`) | Cookie IPC round-trip to the UI process is slow — the UI process is busy or contended. |
| Non-zero `drain delay` (`wire^:`) | We noticed completion later than curl did — event-loop delay between curl's done-message landing and `check_active_requests` running. |
| `wire-batch:` followed by N `wire:` lines with the same timestamp prefix | Cosmetic clustering. The network completion times were spread out; only the logging happened in a tight loop. Trust the per-request phase numbers. |

If `wire+` shows stalls but `wire++` doesn't, the network was steady and
curl's decompressor was batching — not interesting. If `wire++` shows
the stall too, it really happened on the wire and the gap text tells
you when in the transfer it landed.

For a worked example of using these lines to diagnose a 1.5 s "slow
download" that turned out to be a 1062 ms server pause inside a
streaming-SSR response, see the discussion that originally added this
logging.

## Where the code lives

Most of this is implemented in `Services/RequestServer/Request.cpp`:

- `log_network_activity` emits the `wire:` line.
- `record_chunk` populates the decoded-side `WireStats`; `log_chunk_stats`
  emits the `wire+:` line.
- `on_xferinfo` (registered via `CURLOPT_XFERINFOFUNCTION`) populates the
  wire-side fields; `log_chunk_stats` also emits the `wire++:` line.
- `mark_lifecycle_event` records pre-network timestamps from the state
  handlers (`handle_dns_lookup_state`, `handle_retrieve_cookie_state`,
  `notify_retrieved_http_cookie`, `handle_fetch_state`,
  `handle_connect_state`); `log_chunk_stats` also emits the `wire^:` line.
- The per-request `WireStats` entry is created in the `Request`
  constructors (so `created_at` is the true creation time) and removed
  in `~Request`.

Process-wide diagnostic lines:

- `wire-batch:`, `wire-stall:`, `wire-burst:` are all in
  `Services/RequestServer/ConnectionFromClient.cpp`. The stall and burst
  detectors live at file scope as `note_event_tick`, `time_curl_call`,
  and `burst_state_by_client`; they are invoked from each event-handler
  entry point.
- `wire-pipe-pressure:` lives in `Request::write_queued_bytes_without_blocking`
  in `Services/RequestServer/Request.cpp`. It also feeds the back-pressure
  totals appended to `wire^:`.
- `LibDNS wire-dns:` lives in `Libraries/LibDNS/Resolver.h`, in
  `Resolver::lookup` — a `ScopeGuard` around the function body times the
  synchronous portion and classifies the resolution path.
- `UI wire-cookie:` lives in `Libraries/LibWebView/Application.cpp`, in
  the `on_retrieve_http_cookie` callback set up by `launch_request_server`.

All of the above lines are gated by the `REQUESTSERVER_WIRE_DEBUG`
debug macro (defined in `Meta/CMake/all_the_debug_macros.cmake`,
defaults to `ON`). To silence the whole subsystem at compile time,
set it to `OFF` and rebuild. Each individual line additionally
self-suppresses under its own threshold (100 ms event-loop gap,
50 ms curl call, 50 ms back-pressure window, 5 ms DNS sync, 5 ms
cookie jar) so even with the macro on, the log only fires when
something is interesting. Adjust the per-line thresholds in the
source if you want them tighter or louder.
