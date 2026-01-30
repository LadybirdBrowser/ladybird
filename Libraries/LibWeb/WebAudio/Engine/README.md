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

- AudioContext suspend/resume promises and statechange events are gated on timing page acknowledgements after the render thread applies the transition.
- WebAudioClientRegistry owns one WebAudioWorkerSession per AudioContext; timing pages and worklet registration caches are per-context.
- Flow control helpers for coalescing notify fds and transactional binding publication live in FlowControl.h; tunables live in Policy.h.
- There is a debug-only OhNoes passthrough node (Internals-created) that can dump render-thread audio to WAV. It does file I/O on the render thread and is intentionally debug-gated.
- Real-time readback for analyser data and dynamics compressor reduction is done via SharedBufferStream snapshots in WebAudioWorkerSession.
- AudioPlaybackStats values are derived from timing page frame and underrun counters plus AudioContext output latency.
- Realtime AudioWorklet processor exceptions are reported back to WebContent and surface as processorerror events on AudioWorkletNode.
- Realtime registerProcessor() calls are mirrored back to WebContent so AudioWorkletNode construction uses the spec-defined descriptor map.
- AudioWorklet.addModule() resolves only after the render-side module evaluation promise settles and the worker acknowledges it.
- Realtime AudioWorklet processing is synchronous on the render thread (no async queue or sleep-based waiting).
- Render graphs process all nodes, even when outputs are not connected to the destination.
- OfflineAudioContext renderSizeHint sets the per-context render quantum size for offline renders; realtime contexts keep the default size.
- When AudioContext is suspended, the realtime AudioWorklet host still pumps its event loop to service MessagePort tasks even when the current frame does not advance.
- WebAudioWorker uses a shared render thread that mixes per-context output into one device stream.
- The WebAudioWorker render thread schedules rendering based on output ring availability and an EMA of render cost; set WEBAUDIO_PERF_LOG=1 to log the moving average.
- AudioWorklet module evaluation happens on the shared render thread once a session exists (output is already open).
- DelayRenderNode output channel count collapses to mono only when the entire quantum reads from unfilled history; otherwise it follows the input channel count.
- GraphCompiler splits DelayNodes that participate in cycles into delay writer and delay reader processing nodes, and removes remaining cyclic nodes from ordering so their outputs are muted.
- Offline apply_update_offline rebuilds nodes for rebuild-required graph updates so resource changes (for example ConvolverNode buffers) take effect after suspend/resume.
- WaveShaperNode curve changes are rebuild-required updates, and oversampling uses the shared sinc resampler helpers.
- IIRFilterNode coefficients are immutable and are embedded in render graph snapshots.
- OscillatorNode custom PeriodicWave coefficients are embedded in the graph snapshot and applied on the render thread with optional normalization.
- OscillatorRenderNode treats negative frequency values as reverse phase advance, and the Nyquist guard uses the absolute frequency.
- ConvolverRenderNode uses partitioned FFT convolution with overlap-add at the render quantum size.
- AudioServer exposes input device enumeration over IPC so MediaCapture can query device handles.
- AudioServer provides input stream descriptors for microphone capture using the RingStream transport layout.
- WebAudioWorker IPC now includes session-level audio input stream creation/destruction and MediaStream audio source binding metadata for AudioServer-backed capture streams.
- AudioWorklet.addModule resolves only after module evaluation is acknowledged and the required processor registration generation is observed.
- GraphNode encode/decode/update helpers assert control-thread access to keep control-plane usage explicit.
- GraphNodeType names follow AudioNode naming (AudioBufferSource, MediaElementAudioSource, MediaStreamAudioSource), and GraphDescription mapping is generated from the GraphNodeTypes macro.
- MediaStreamAudioDestinationNode currently snapshots as a disabled OhNoes graph node, so it does not publish a stream transport yet.
- Media element taps are captured at the device sample rate and resampled to the AudioContext rate in the MediaElementAudioSourceRenderNode.

## Guidelines

- When a producer has pushed the last decoded media frames, signal end-of-stream promptly to avoid misclassifying end-of-clip underruns as starvation.
