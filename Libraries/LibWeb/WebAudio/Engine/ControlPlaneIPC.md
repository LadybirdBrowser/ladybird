# WebAudio out-of-process IPC (WebContent <-> WebAudioWorker)


This file is about the control plane: processes, IPC methods, and how session state is applied.

The shared-memory data-plane primitives (RingStream, notify fds, and SharedBufferStream bindings) are described in DataPlaneTransport.md.

## Resource boundary

Render graph payloads carry IDs, not process-local objects.

The resolution boundary for external resources is shared across execution models:
- Libraries/LibWeb/WebAudio/Engine/GraphResources.h (Render::GraphResourceResolver)

## Session lifecycle

WebContent calls into the WebAudio server surface defined by Services/WebAudioWorker/WebAudioServer.ipc.

- create_webaudio_session(target_latency_ms)
  - Creates per-session state.
  - Returns a session_id plus the shared timing page and its notify fd.

- destroy_webaudio_session(session_id)
  - Tears down the session.

Teardown behavior is owned by the WebAudioWorker broker; when the last broker connection closes, the helper can exit.

## Render graph updates

- webaudio_session_set_render_graph(session_id, encoded_graph)
  - WebContent encodes a RenderGraphDescription using the internal wire codec.
  - WebAudioWorker decodes and swaps graph state for the render thread using atomic handoff (pending -> active) to avoid mutexes on the render thread.

Wire format notes:
- The wire format is an internal same-build ABI.
- Engine/GraphCodec.cpp/h is the entry point.
- Adding a new node to the out-of-process backend typically requires a GraphNodes/<Node>GraphNode implementation plus codec support.

## Suspend/resume acknowledgement

The timing page includes a packed suspend state (suspended bit plus generation token).
WebContent uses it to resolve suspend/resume promises only after the render thread has applied the transition.

Control-plane message:
- webaudio_session_set_suspended(session_id, suspended, generation)

## ScriptProcessor bindings

- webaudio_session_set_script_processor_streams(session_id, streams)
  - WebContent allocates the shared streams and publishes the descriptors.
  - WebAudioWorker bridges the request/response streams via SessionScriptProcessorHost without blocking the render thread.

## Worklet bindings

- webaudio_session_add_worklet_module(session_id, url, source_text)
  - Pushes module source text into the session.

- webaudio_session_set_worklet_node_ports(session_id, ports)
  - Sends processor-side MessagePort fds so the realtime host can attach them.

Worklet node definitions carry outputChannelCount as an optional list so the control plane can preserve the difference between not provided and an empty list.
Render graph snapshots include all nodes, so worklet processing does not depend on destination reachability or control-plane flags.
Realtime AudioWorklet processing runs synchronously on the render thread; process() completes within the render quantum.
When the context is suspended, the render thread may still pump the worklet event loop to service MessagePort tasks even when the current frame does not advance.

## Worklet error notifications

- webaudio_session_worklet_processor_error(session_id, node_id)
  - Sent from WebAudioWorker to WebContent when an AudioWorkletProcessor throws from process().
  - WebContent dispatches processorerror on the matching AudioWorkletNode.

## Worklet processor registration notifications

- webaudio_session_worklet_processor_registered(session_id, name, descriptors, generation)
  - Sent from WebAudioWorker to WebContent when registerProcessor() succeeds in the worker realm.
  - WebContent updates the control-thread AudioWorklet descriptor map via a media element task so AudioWorkletNode construction is spec compliant.
  - The generation value is echoed onto MessagePort traffic so control-thread delivery can be deferred until registration updates are applied.

## Realtime snapshot streams

- webaudio_session_create_analyser_stream(session_id, analyser_node_id, fft_size, block_count)
  - Allocates a SharedBufferStream for analyser snapshots (time-domain and frequency data).

- webaudio_session_create_dynamics_compressor_stream(session_id, compressor_node_id, block_count)
  - Allocates a SharedBufferStream for per-quantum dynamics compressor reduction readback.

## Known gaps / constraints

- Most session IPC methods return void; failures are primarily surfaced via logs.
- Render graph update error reporting is not plumbed back to WebContent.
- Worklet compile and constructor errors are not yet plumbed back to WebContent for realtime contexts.
