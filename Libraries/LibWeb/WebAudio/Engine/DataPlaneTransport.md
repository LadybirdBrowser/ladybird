# WebAudio stream transports

BEWARE OUT OF DATE!

This file documents the current shared-memory data-plane mechanisms used by the WebAudio out-of-process backend.

## Design constraints

- Render thread must not block.
- Steady state should avoid locks and allocations.
- Data plane is shared memory plus atomics; IPC is used to create/bind transports.

## Building blocks

1) RingStream (interleaved f32 audio frames)

Use RingStream for high-rate audio frame transport across processes.

Code:
- Libraries/LibWebAudio/LibWebAudio.h (shared-memory layout + atomic helpers)
- Libraries/LibWebAudio/RingStream.h (non-blocking producer/consumer + descriptor validation)
- Libraries/LibWebAudio/LibWebAudio.h (control-plane descriptors)

Shared memory layout:
- RingStreamHeader followed by interleaved f32 sample data.
- Counters are monotonic frame indices.
- SPSC: read_frame is consumer-written; write_frame is producer-written.

Overrun behavior:
- The consumer detects overruns (write - read > capacity) and fast-forwards read.
- overrun_frames_total is maintained by the consumer as a diagnostic.

End-of-stream:
- ring_stream_flag_end_of_stream is a bit in the header flags word.

Optional timeline metadata:
- timeline_generation is a discontinuity token.
- timeline_sample_rate and the media start fields allow a consumer to derive a best-effort start time for the current read cursor.

Overflow behavior:
- RingStreamProducer keeps the newest data by advancing the effective write position when the ring is full.

2) SharedBufferStream (discrete blocks)

ScriptProcessor uses SharedBufferStream for request/response block transport.

The WebAudio control-plane carries SharedBufferStreamDescriptor (three shared buffers) plus a notify write fd.

Code:
- Libraries/LibWebAudio/SharedBufferStream.h
- Libraries/LibWebAudio/LibWebAudio.h (SharedBufferStreamDescriptor)

3) Notify fds (eventfd / pipe)

Notify fds are wakeup hints.

- On Linux, the preferred primitive is eventfd (non-blocking + close-on-exec).
- Elsewhere, a non-blocking pipe is used.

Code:
- InputStream.cpp / callsites (nonblocking notify fd pair creation)
- Libraries/LibWebAudio/LibWebAudio.h / LibWebAudio.cpp (signal + drain helpers)

Important semantics:
- Notify fds coalesce naturally. Writers may signal more than once; readers must re-check shared state.

## Where these are used

- MediaElementAudioSource bridging:
  - Producer (WebContent): MediaElementAudioSourceProvider publishes a RingStream and signals notify.
  - Consumer (WebAudioWorker): WebAudioSession validates the descriptor and attaches a MediaElementAudioSourceProvider in remote-consumer mode.

- ScriptProcessor transport:
  - WebContent allocates SharedBufferStream request/response streams and publishes descriptors.
  - WebAudioWorker bridges the streams in SessionScriptProcessorHost.

- WaveShaperNode:
  - Uses regular render graph updates and does not allocate new data-plane transports.

- Audio input capture (AudioServer -> WebAudioWorker):
  - AudioServer publishes a RingStream-backed shared-memory transport for an input device.
  - WebContent publishes MediaStream audio source metadata (device id, sample rate, channel count).
  - WebAudioWorker requests the AudioServer stream and binds the provider id to the shared RingStream.
  - The AudioServer stream descriptor is defined in Services/AudioServer/AudioInputStream.h and
    uses the same RingStream header layout as WebAudio's transport helpers.

- MediaStreamAudioDestinationNode output:
  - Not wired yet. There is no shared-memory transport for destination node streams at this time.
- OfflineAudioContext rendering is in-process and does not use shared-memory transports; it can choose a per-context render quantum size via renderSizeHint.
- WebAudioWorker render thread schedules rendering based on output ring availability and an EMA of render cost; WEBAUDIO_PERF_LOG enables periodic logging.
- Timing page shared memory is separate from stream transports and publishes rendered frame and underrun counters for AudioPlaybackStats.

## Potential reuse outside WebAudio

The notify-fd drain/signal helpers are not WebAudio-specific.
If we need the same primitive in other subsystems, the helpers currently in LibWebAudio can be moved under LibCore with a more general name.
