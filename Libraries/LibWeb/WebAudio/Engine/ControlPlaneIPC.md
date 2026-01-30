# WebAudio out-of-process IPC (WebContent <-> WebAudioWorker)


This file is about the control plane: processes, IPC methods, and how session state is applied.

The shared-memory data-plane primitives (RingStream, notify fds, and SharedBufferStream bindings) are described in DataPlaneTransport.md.

## Resource boundary

Render graph payloads carry IDs, not process-local objects.

The resolution boundary for external resources is shared across execution models:
- Libraries/LibWeb/WebAudio/Engine/GraphResources.h (Render::GraphResourceResolver)
  - AudioBuffer resources are snapshotted from AudioBuffer channel data; if any channel data buffer is detached, the snapshot treats the entire buffer as silent.
  - Most render node coefficients are embedded directly in the graph snapshots.
  - ConvolverNode impulse responses are snapshotted into shared audio buffers; partitioning and FFT convolution happen on the render thread without additional IPC payloads.

## Session lifecycle

WebContent calls into the WebAudio server surface defined by Services/WebAudioWorker/WebAudioServer.ipc.

Each AudioContext owns its own WebAudioSession in the helper. A single WebAudioWorker process per page can host multiple
sessions concurrently on a shared render thread, mixing into one output stream.

- create_webaudio_session(target_latency_ms)
  - Creates per-session state and ensures the shared output device is open.
  - Returns a session_id plus the per-session timing page and its notify fd.

- destroy_webaudio_session(session_id)
  - Tears down the session.

Teardown behavior is owned by the WebAudioWorker broker; when the last broker connection closes, the helper can exit.

## Render graph updates

  - WebContent encodes a RenderGraphDescription using the internal wire codec.
  - WebAudioWorker decodes and swaps graph state for the render thread using atomic handoff (pending -> active) to avoid mutexes on the render thread.

- WebAudioWorker render thread uses an EMA of render cost to schedule rendering against output ring availability; WEBAUDIO_PERF_LOG enables periodic logging.

## Suspend/resume acknowledgement

The timing page includes a packed suspend state (suspended bit plus generation token).
WebContent uses it to resolve suspend/resume promises and to drive control-thread statechange events only after the render thread has applied the transition.

Control-plane message:

## ScriptProcessor bindings

- webaudio_session_set_script_processor_streams(session_id, streams)
## MediaStream audio source bindings

- webaudio_session_set_media_stream_audio_source_streams(session_id, streams)
  - WebAudioWorker asks AudioServer to create input capture streams and binds provider ids to RingStream transports.
  - MediaStreamAudioDestinationNode does not publish a transport yet, so there is no control-plane IPC for stream output.

- webaudio_session_add_worklet_module(session_id, module_id, url, source_text)

- webaudio_session_set_worklet_node_ports(session_id, ports)
  - Sends processor-side MessagePort fds so the realtime host can attach them.

Worklet node definitions carry outputChannelCount as an optional list so the control plane can preserve the difference between not provided and an empty list.
Render graph snapshots include all nodes, so worklet processing does not depend on destination reachability or control-plane flags.
When the context is suspended, the render thread may still pump the worklet event loop to service MessagePort tasks even when the current frame does not advance.

## Worklet error notifications

- webaudio_session_worklet_processor_error(session_id, node_id)
  - WebContent dispatches processorerror on the matching AudioWorkletNode.

## Worklet processor registration notifications

- webaudio_session_worklet_processor_registered(session_id, name, descriptors)
  - Sent from WebAudioWorker to WebContent when registerProcessor() succeeds in the worker realm.
  - Includes a registration generation token so addModule resolution can be ordered against processor registration.

## Worklet module evaluation notifications

- webaudio_session_worklet_module_evaluated(session_id, module_id, required_generation, success, error_message, failed_processor_registrations)
  - Sent from WebAudioWorker to WebContent when a worklet module evaluation promise settles in the render host.
  - Failed processor registrations are cached so AudioWorkletNode construction throws InvalidStateError for invalid descriptors.

- webaudio_session_create_analyser_stream(session_id, analyser_node_id, fft_size, block_count)
  - Allocates a SharedBufferStream for analyser snapshots (time-domain and frequency data).
  - Allocates a SharedBufferStream for per-quantum dynamics compressor reduction readback.

- webaudio_session_create_audio_input_stream(session_id, device_id, sample_rate_hz, channel_count, capacity_frames, overflow_policy)
  - Requests AudioServer to create a shared RingStream for microphone capture and returns a stream id.

Most session IPC methods return void; failures are primarily surfaced via logs.

## Related AudioServer IPC

Audio input device enumeration is handled by AudioServer, not WebAudioWorker. The AudioServer IPC surface provides get_audio_input_devices so WebContent can query input devices via LibMedia and later bind a device handle to a capture stream.

AudioServer also exposes create_audio_input_stream and destroy_audio_input_stream to publish shared-memory RingStream transports for microphone capture. WebAudioWorker will use these descriptors when MediaStream audio sources are wired up.
