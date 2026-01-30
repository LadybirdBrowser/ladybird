# WebAudio Catalog

## Engine (WebContent-side)

- WebAudioClientRegistry.cpp/h
  - WebAudio control-plane coordinator in WebContent.
  - It owns per-context session wiring to the selected render backend and manages timing page polling for
  currentTime/suspend acks.

- Policy.h
  - Centralized compile-time tunables used by flow-control and retry logic (for example ScriptProcessor binding publish retries).

- FlowControl.h
  - Small reusable helpers used by backends to coalesce notification fds and to publish bindings transactionally (publish complete bindings, or retry later).

- GraphCodec.cpp/h
  - Internal same-build wire format for RenderGraphDescription. Relevant mostly adding new node types.

- OfflineAudioContext.cpp
  - Offline render loop and worklet/script processor integration.

- SharedMemory.h
  - Timing page layout and small snapshot headers used by WebContent for real-time readback.

## Stream transports (data plane)

- StreamTransport.h / StreamTransportRing.h
  - Shared-memory RingStream layout and non-blocking SPSC helpers for interleaved f32 audio frames.
  - Used for high-rate transports (for example MediaElementAudioSource bridging).

- StreamTransportDescriptors.h / StreamTransportIPC.h / StreamTransportValidation.h
  - Control-plane descriptors and IPC encoding/decoding for RingStream and other transport bindings.

## Per-node descriptions

- GraphNodes/**
  - Mostly flat structs that bridge the gap between AudioNodes and RenderNodes by encoding/decoding to the
  wire protocol used to communicate audio graph updates to WebAudioWorker. 

- GraphNodes/GraphNodeTypes.h
  - Types used across snapshotting, encoding/decoding, compilation, and execution.

## Render-thread nodes

- RenderNodes/**
  - Render-thread implementations of AudioNodes.
  - RenderNode onstructors and destructors may allocate/free, process methods must not.

## Services/WebAudioWorker (out-of-process backend)

- WebAudioServer.ipc
  - Control-plane surface between WebContent and WebAudioWorker.
  - This is the place to look when you need to add a new session-level binding or an acknowledgement path.

- WebAudioWorkerSession.cpp/h
  - WebContent's side of the bridge to WebAudioSession. Abstracts graph & stream transport.

- WebAudioSession.cpp/h
  - The WebAudioWorker process's session coordinator: graph swapping, stream binding application, worklet host wiring, and timing page writes.

- Worklet/RealtimeAudioWorkletProcessorHost.cpp
  - Realtime AudioWorklet host used by the render thread to evaluate modules and run processor process() synchronously.
