# Engine

For a broad, plain-language architecture overview (process model, thread split, major components), see:
- Documentation/WebAudio.md

Engine is WebAudio's non-IDL implementation layer: graph compilation/execution, DSP helpers, backend wiring, and shared-memory transport helpers.

If you're trying to understand the out-of-process backend:
- IPC surface and session messages: ControlPlaneIPC.md
- Shared-memory stream transports: DataPlaneTransport.md

## What lives here

- Graph execution orchestration (quantum loop ownership, scheduling, lifecycle).
- Applying graph snapshots/updates to the running render graph.
- Backend integration points (in-process vs WebAudioWorker).
- Shared-memory transport primitives (RingStream layout, notify fds, validation).

Node-specific render descriptions live under WebAudio/GraphNodes.

## Non-obvious behaviors worth noting (beware out of date!)

- AudioContext suspend/resume promises are resolved only after the render thread applies the transition (timing page acknowledgement).
- Flow control helpers for coalescing notify fds and transactional binding publication live in FlowControl.h; tunables live in Policy.h.
- There is a debug-only OhNoes passthrough node (Internals-created) that can dump render-thread audio to WAV. It does file I/O on the render thread and is intentionally debug-gated.
- Real-time readback for analyser data and dynamics compressor reduction is done via SharedBufferStream snapshots in WebAudioWorkerSession.
- Realtime AudioWorklet processor exceptions are reported back to WebContent and surface as processorerror events on AudioWorkletNode.
- Realtime registerProcessor() calls are mirrored back to WebContent so AudioWorkletNode construction uses the spec-defined descriptor map.
- MessagePort delivery for AudioWorklet nodes is generation-gated so posted messages wait until matching registerProcessor() updates are applied on the control thread.
- Realtime AudioWorklet processing is synchronous on the render thread (no async queue or sleep-based waiting).
- Render graphs process all nodes, even when outputs are not connected to the destination.
- When AudioContext is suspended, the realtime AudioWorklet host still pumps its event loop to service MessagePort tasks even when the current frame does not advance.

## Guidelines

- When a producer has pushed the last decoded media frames, signal end-of-stream promptly to avoid misclassifying end-of-clip underruns as starvation.
