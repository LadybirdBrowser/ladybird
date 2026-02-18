# Intro to AudioServer

This PR moves the OS audio device layer out of LibMedia and into a dedicated AudioServer process. It cuts down on OS playback threads, it enumerates devices, allows device selection, adds input streams, and removes direct audio device access from WebContent. The security payoff is not immediate and probably not uniform across platforms, but this is a step towards a less privileged WebContent process.

### TLDR if you're familiar with the old LibMedia audio playback

- AudioMixingSink still mixes tracks.
- PlaybackStream code is moved to Services/AudioServer & Services/AudioServer/Platform.
- AudioServer now implements the playback stream contract with a single callback thread per output device.
- Shared memory ring buffers replaces direct in-process sample callbacks across the process boundary.
- The TimingReader class provides playback timing data using a per-stream shared memory buffer.

Before this change, an active AudioMixingSink created its own PlaybackStream, and each PlaybackStream owned its own backend-specific device callback machinery. In practice that meant a callback thread and direct path to CoreAudio, PulseAudio, or WASAPI for each active sink.

With this change, PlaybackStream is gone from LibMedia. WebContent still mixes media tracks in process through AudioMixingSink, but it no longer opens or drives the OS device. Instead it connects to AudioServer, asks for an output session, and writes mixed float samples into a shared ring buffer. AudioServer owns the real device-facing code, keeps one actual callback path per output device, pulls from all producers for that device, mixes them together, and hands the result to the platform backend.

---

### Process layout

There are three distinct roles.

The browser process owns the singleton AudioServer process and acts as the broker for new clients. When a WebContent process is created, the browser asks AudioServer for a fresh client connection, and passes that connection along to the WebContent.

Each WebContent process owns its own session connection to AudioServer. That connection is used for device enumeration, output session creation, output volume requests, and input stream requests.

AudioServer owns the platform backends and the shared device state. It is the only process that talks to CoreAudio, PulseAudio, or WASAPI. It also owns the per-device mixer. If two WebContent processes send audio to the same output device, they become two producers feeding one server-side device stream.

---

### Data flow and timing

The new output path is pull-driven by the real output device.

1. WebContent asks AudioServer to create an output session, optionally for a specific device.
2. AudioServer creates shared memory for sample data and timing data, then returns those handles to the client.
3. AudioMixingSink writes mixed samples into the shared ring buffer.
4. The platform callback thread in AudioServer wakes when the device needs more data.
5. AudioServer pulls from every producer attached to that device, mixes the available samples, and submits the mixed result to the OS backend.
6. AudioServer publishes timing information such as played frames, ring consumption, and underrun count into a second shared buffer.
7. AudioMixingSink reads those timing snapshots and uses them to report media time.

The client is expected to keep the ring buffer fed. Audio timing is derived from the server's device-facing state rather than from a local PlaybackStream object. A shared-memory struct uses atomic fields and a sequence counter (per output stream) to give each client timing snapshots without locking the audio callback thread.

This timing buffer is exposed to clients through the TimingReader wrapper. The client attaches it once when the output session is created and then calls read_snapshot whenever it wants fresh timing data. In practice the main useful value is device_played_frames, which tells the client how far this session has really made it through the device so AudioMixingSink can turn that back into media time.

The same split exists for input. AudioServer enumerates input devices, creates the platform input stream, fills a shared ring, and exposes that ring to the client. The grant id on the session is what allows AudioServer to decide whether a given client may open microphone input at all.

---

### Device enumeration and updates

Device enumeration now lives with the same process that owns the real device APIs. AudioServer queries the platform backend, turns the result into a cross-platform DeviceInfo list, caches that list, and serves it to session clients through get_devices.

The service also listens for device changes directly. The platform backends register native listeners for add, remove, and default-device changes. When one of those callbacks fires, AudioServer re-enumerates devices, compares the new list to its cached list, replaces the cache if it changed, and then proactively notifies every connected session client with notify_devices_changed.

That push model matters because it means WebContent does not need to wait for the next navigator.mediaDevices request before learning that the world changed. Each connected client gets an immediate signal that its device cache is stale and can refresh it ahead of the next user-visible query. The result is that later device-list requests can be served from a fresh cached view instead of having to rediscover the platform state on demand.

---

# Appendix

### Why are the IPC files named this way?

The IPC definitions use names like ToAudioServerFromSessionClient.ipc and ToSessionClientFromAudioServer.ipc.

That is different from the shorter naming used in much of the existing tree. The benefit of the longer form is that it says both who implements the interface and who is expected to send the messages. In this PR there are two distinct client edges into AudioServer: the browser-side broker connection and the per-WebContent session connection. Naming both recipient and sender makes those edges explicit and makes it harder to confuse the browser-facing broker messages with the session-facing audio messages.

It also maps cleanly onto the actual connection types. BrokerOfAudioServer, SessionClientOfAudioServer, BrokerConnection, and SessionConnection are separate classes with separate message sets, and the filenames reflect that split.

What this naming does not do is create modularity by itself. The code would still need clear connection boundaries even with shorter filenames. So the real argument for the scheme is clarity about direction and connection role, not that it unlocks some IPC feature that the usual naming cannot express.

### Why is LibAudioServer linked by all three of Ladybird, WebContent, and AudioServer?

LibAudioServer is not server-only code in the narrow sense. It is the shared edge library for the whole feature.

It contains the data structures that both sides of a session need to agree on, the IPC endpoint definitions, the connection classes used by the browser and WebContent side, and the helpers for passing audio and timing data. Linking that library from multiple binaries is intentional. It keeps the audio-specific shared code in one place instead of scattering headers and helpers across multiple libraries or duplicating them at the process edges.

This is not the most common pattern elsewhere in Ladybird. But pushing client/server boundary code towards the middle isolates more of the audio-specific coupling inside one library, which seems better than spreading that coupling across LibMedia, LibWebView, and Services.
