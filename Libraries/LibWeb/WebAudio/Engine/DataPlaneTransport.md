# WebAudio stream transports

This file documents the current shared-memory data-plane mechanisms used by the WebAudio out-of-process backend.

## Design constraints

- Render thread must not block.
- Steady state should avoid locks and allocations.
- Data plane is shared memory plus atomics; IPC is used to create/bind transports.

## Building blocks

1) RingStream (interleaved f32 audio frames)

Use RingStream for high-rate audio frame transport across processes.

Code:
- StreamTransport.h (shared-memory layout + atomic helpers)
- StreamTransportRing.h (non-blocking producer/consumer helpers)
- StreamTransportDescriptors.h (control-plane descriptors)

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

Overflow policy:
- StreamOverflowPolicy is part of the descriptor.
- StreamTransportRing.h implements DropOldest (overwrite-by-advancing effective write position).
- For other policies, the current producer helper just short-writes when full.
  Lossless waiting/backpressure is owned by higher layers (not implemented inside RingStreamProducer).

2) SharedBufferStream (discrete blocks)

ScriptProcessor uses Core::SharedBufferStream for request/response block transport.

The WebAudio control-plane carries SharedBufferStreamDescriptor (three shared buffers) plus a notify write fd.

Code:
- Libraries/LibCore/SharedBufferStream.h
- Libraries/LibWeb/WebAudio/Engine/StreamTransportDescriptors.h (SharedBufferStreamDescriptor)

3) Notify fds (eventfd / pipe)

Notify fds are wakeup hints.

- On Linux, the preferred primitive is eventfd (non-blocking + close-on-exec).
- Elsewhere, a non-blocking pipe is used.

Code:
- StreamTransportEventFD.h (create_nonblocking_stream_notify_fds)
- StreamTransportNotify.h (signal + drain helpers)
- FlowControl.h (drain helper that also detects closed/broken)

Important semantics:
- Notify fds coalesce naturally. Writers may signal more than once; readers must re-check shared state.

## Where these are used

- MediaElementAudioSource bridging:
  - Producer (WebContent): MediaElementAudioSourceProvider publishes a RingStream and signals notify.
  - Consumer (WebAudioWorker): WebAudioSession validates the descriptor and attaches a MediaElementAudioSourceProvider in remote-consumer mode.

- ScriptProcessor transport:
  - WebContent allocates SharedBufferStream request/response streams and publishes descriptors.
  - WebAudioWorker bridges the streams in SessionScriptProcessorHost.

## Potential reuse outside WebAudio

The notify-fd helpers (eventfd/pipe creation plus drain/signal helpers) are not WebAudio-specific.
If we end up needing the same primitive in other subsystems, StreamTransportEventFD.h and StreamTransportNotify.h are candidates to move under LibCore with a more general name.
