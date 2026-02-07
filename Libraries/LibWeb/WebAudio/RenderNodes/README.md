# RenderNode

This folder contains render-thread node implementations that mirror the
IDL-facing AudioNode classes.

The basename disambiguation is intentional:
- OscillatorNode is the main-thread IDL object.
- OscillatorRenderNode is the render-thread counterpart.

RenderNodes run on the realtime render thread. Keep their steady-state execution lock-free and allocation-free, and prefer atomic publication patterns for any injected host pointers.
Render graphs process all nodes, even when outputs are not connected to the destination, so render nodes should tolerate being invoked without downstream consumers.

AudioWorkletRenderNode derives its per-quantum output channel counts from channelCount and channelCountMode when outputChannelCount is not provided.

If you need a debugging-only node that is not realtime-safe (for example it does file IO or allocations), keep it behind debug gating and avoid exposing it to normal JS surfaces. Prefer Internals-only factories for that kind of instrumentation.

Small cross-cutting helpers that are used by multiple subsystems (for example param index layouts) live alongside the RenderNodes in this folder.

## Adding a new RenderNode

This is a practical checklist for adding a new render-thread node type end-to-end (control-thread snapshot -> wire encoding -> render-thread execution). The AudioBufferSource node is a good reference because it exercises both AudioParams and an external resource payload (AudioBuffer PCM).

### 1. Define the node description (GraphNode)

- Add an entry to `GraphNodeType` in `GraphNodes/GraphNodeTypes.h`.
- Add `GraphNodes/YourNodeGraphNode.h` / `.cpp`.
	- This struct is the canonical per-node render description.
	- Include only render-thread safe state: plain numbers, enums, and handles.
	- For heavy/process-local objects, store a stable handle (for example `buffer_id`) and keep the actual data in the resource registry.
	- Implement: wire encode/decode, update classification, render-node factory, and AudioParam clamp/intrinsic helpers.

- In `Engine/GraphDescription.h`:
	- Add `YourNodeGraphNode` to the `GraphNodeDescription` Variant.
	- Ensure `graph_node_type(GraphNodeDescription const&)` covers your Variant alternative.

### 2. Define AudioParam layout (if the node has params)

- Add a param-count entry to `GraphNodes/GraphNodeTypes.h`.
- Add a per-node `*ParamIndex` struct with stable per-node indices.
- Ensure the snapshot builder uses the same indices when registering params and recording param connections.

### 3. Implement the render node

- Add `RenderNodes/YourNodeRenderNode.h` / `.cpp` implementing `Render::RenderNode`.
- Implement at least:
	- `process()`
	- `output(index)`
	- `apply_description(GraphNodeDescription const&)`
	- Any scheduling hooks your node needs (`schedule_start`, `schedule_stop`) if it behaves like an AudioScheduledSourceNode.
- Keep update classification in the GraphNode (`YourNodeGraphNode::classify_update`) so the compiler/executor can reason about safety without duplicating logic.
	- Use `node.has<YourNodeGraphNode>()` and `node.get<YourNodeGraphNode>()` inside `apply_description()`.

### 4. Snapshot the main-thread node into a RenderGraphDescription

- Update `WebAudio/RenderGraphSnapshot.cpp` to recognize the IDL-facing AudioNode type and set the corresponding `GraphNodeDescription` Variant alternative.
- If the node depends on heavy data (for example AudioBuffer PCM), publish it via the `GraphResourceRegistry` and only reference it by handle from the node description.
- Register AudioParams for the node so automation and param connections are captured.

### 5. Wire format support (AudioServer backend)

- Update `Engine/GraphCodec.cpp`:
	- Encode/decode the node payload by calling `YourNodeGraphNode::encode_wire_payload` / `decode_wire_payload`.
	- Construct the correct `GraphNodeDescription` Variant alternative during decode.
	- If your node references out-of-band resources, set `WireFlags::contains_external_resources` and emit (or decode) the relevant resource table section.

### 6. Graph compilation and execution

- Update `Engine/GraphCompiler.cpp`:
	- Instantiate the new `RenderNode` in the node factory (typically via `GraphNodeDescription::visit()`).
	- Add update classification logic so graphs can apply realtime parameter-only updates when safe.
- Update `Engine/GraphExecutor.cpp`:
	- If the node has AudioParams, seed per-param intrinsic state (initial/default/min/max) from the node description so clamping does not pin values to zero.

### 7. Render-thread readback (optional)

If the node exposes realtime state to the control thread (for example analyser data or dynamics compressor reduction), add a SharedBufferStream snapshot format in Engine/SharedMemory.h and wire it through WebAudioWorkerSession and the AudioService query path.
