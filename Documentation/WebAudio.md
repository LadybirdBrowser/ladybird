# WebAudio in Ladybird: a practical architecture overview

This document explains how WebAudio works in this fork.

If you are adding a new node type or plumbing a new transport, see the more
implementation-focused docs under the engine directory:
- [Adding a node](../Libraries/LibWeb/WebAudio/RenderNodes/README.md)
- [Engine overview](../Libraries/LibWeb/WebAudio/Engine/README.md)
- [Control-plane IPC](../Libraries/LibWeb/WebAudio/Engine/ControlPlaneIPC.md)
- [Data-plane transports](../Libraries/LibWeb/WebAudio/Engine/DataPlaneTransport.md)

## Big picture

The Web Audio spec talks about a "control thread" and a "rendering thread":
- https://webaudio.github.io/web-audio-api/#control-thread
- https://webaudio.github.io/web-audio-api/#rendering-thread

In Ladybird terms:

Control thread: where JavaScript calls land.
- Creates nodes, connects them, schedules starts/stops, edits parameters.
- Owns promises (suspend/resume/start rendering) and user-observable state.

Render thread: where audio is produced.
- Runs the graph in fixed-size chunks (a "render quantum", currently 128 frames).
- Must be fast and predictable.
- Produces audio on a tight deadline (DSP, mixing, realtime work).

The render side is intentionally isolated from the page. That decision is motivated by
making AudioWorklet a first-class feature: the worklet code needs a place to run that is
"always there", predictable, and not constantly fighting the page's main thread.

### At a glance (process model)

```
                      +---------------------------------+
                      |  Ladybird process (the broker)  |
                      +----+-----------------------+----+
                           |                       ^
                           |                    <spawn>
                           |                       v
                           |        +--------------+----+
                           |        |  WebContent       |
                        <spawn>     |  control thread   |
                           |        |  script processor |
                           |        +----+---------+----+
                           |             ^         |
                           |         <control>     |
                           |          <data>       |
                           v             v         |
                      +----+-------------+----+    |
                      |  WebAudioWorker       |    |
                      |  render thread        |    |
                      |  worklets             |    |
                      +----+------------------+    |
                           |                       |
                           |              <media element audio>
                           |                       |
                           | <shared ring buffers> |
                           v                       v
                      +----+-----------------------+----+
                      |  AudioServer (device + mixing)  |
                      +---------------------------------+
```

In code, those pieces map roughly to:

- WebContent-side API surface:
  - [AudioNode](../Libraries/LibWeb/WebAudio/AudioNode.cpp) and friends
  - [BaseAudioContext](../Libraries/LibWeb/WebAudio/BaseAudioContext.cpp)
  - [AudioContext](../Libraries/LibWeb/WebAudio/AudioContext.cpp)
  - [OfflineAudioContext](../Libraries/LibWeb/WebAudio/OfflineAudioContext.cpp)
  - [ScriptProcessor](../Libraries/LibWeb/WebAudio/ScriptProcessor)

- WebContent-side bridge to the WebAudioWorker backend:
  - [AudioService (process-global coordinator)](../Libraries/LibWeb/WebAudio/AudioService.cpp)
  - [WebAudioClientRegistry (session and timing coordinator)](../Libraries/LibWeb/WebAudio/Engine/WebAudioClientRegistry.cpp)
  - [WebAudioWorkerSession (owns the helper session + transports)](../Libraries/LibWeb/WebAudio/Engine/WebAudioWorkerSession.cpp)

- Render process (WebAudioWorker):
  - [WebAudioSession (render loop + graph swap + worklet host)](../Services/WebAudioWorker/WebAudioSession.cpp)
  - [WebAudioWorker control connection](../Services/WebAudioWorker/WebAudioWorkerConnection.cpp)
  - [WebAudioServer control-plane IPC surface](../Services/WebAudioWorker/WebAudioServer.ipc)

- Device process (AudioServer):
  - [Audio output device abstraction](../Services/AudioServer/AudioOutputDevice.cpp)
  - [AudioServer IPC surface](../Services/AudioServer/AudioServerServer.ipc)

## Why a helper process?

In addition to the worklet concerns touched on above, realtime audio can be unforgiving.
If the audio callback misses its deadline, you get dropouts, crackles, and "why does this
sound broken" bugs that are miserable to debug.

Keeping the render loop out of WebContent gives us:

- A dedicated render thread with a stable priority and a clear job.
- A natural place to host AudioWorklet processors without tying them to the page's main
  thread.
- Process boundaries help us enforce simpler rules for what can run on the render thread
  (and what absolutely must not).

### "Renderland" rules (realtime constraints)

Render-thread code has a different set of rules than normal application code.
The guiding principle is: do not do anything that might block, allocate, or surprise the
scheduler.

Render-thread node implementations live in the RenderNodes directory:
- [RenderNodes overview](../Libraries/LibWeb/WebAudio/RenderNodes/README.md)
- Example: [GainRenderNode](../Libraries/LibWeb/WebAudio/RenderNodes/GainRenderNode.cpp)

Graph execution is run by the engine layer:
- [GraphExecutor](../Libraries/LibWeb/WebAudio/Engine/GraphExecutor.cpp)

### Dos and donts for the render thread

Do not allocate or free heap memory in steady state.
- No growthy Vector operations, no GC, no "just one more allocation".
Avoid locks and blocking.
- No mutexes, no condition variables, no waits.
Avoid syscalls with unbounded latency.
- No filesystem or network access, no IPC, no non-debug logging.
Keep work bounded.
- No unbounded loops, avoid recursion.

## The render graph: snapshots, compilation, and execution

From the page's point of view, WebAudio is a graph of nodes and connections.
From the render thread's point of view, it is a compact description that can be executed
quickly.

Ladybird bridges that gap by snapshotting the IDL-facing objects into a render description
and then running that description on the render thread.

### Important pieces:

Per-node render descriptions ("what the render thread needs to know"):
- [GraphNodeTypes](../Libraries/LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h)
- [GraphNodes](../Libraries/LibWeb/WebAudio/GraphNodes/)

Building the snapshot (control thread):
- [RenderGraphSnapshot](../Libraries/LibWeb/WebAudio/RenderGraphSnapshot.cpp)

The description format and wire codec:
- [GraphDescription](../Libraries/LibWeb/WebAudio/Engine/GraphDescription.h)
- [GraphCodec (encode/decode)](../Libraries/LibWeb/WebAudio/Engine/GraphCodec.cpp)

Compilation and execution (render side):
- [GraphCompiler](../Libraries/LibWeb/WebAudio/Engine/GraphCompiler.cpp)
- [GraphExecutor](../Libraries/LibWeb/WebAudio/Engine/GraphExecutor.cpp)

### Why snapshot?

The snapshot is a "frozen" view of the graph that only contains render-safe data:

- Plain numbers and small structs.
- Stable IDs instead of pointers.
- Handles to external resources (like decoded PCM) rather than owning heavyweight objects.

That keeps the render thread simple: it does not need to touch DOM objects, GC things, or
page state.

## IPC and transports: control plane vs data plane

There are two kinds of traffic between the page and the helper:

1) Control plane (low rate, structured messages)

This includes:
- "Here is a new graph snapshot"
- "Suspend or resume"
- "Bind this ScriptProcessor transport"
- "Bind this worklet port"

The control-plane surface is
[WebAudioServer.ipc](../Services/WebAudioWorker/WebAudioServer.ipc), with rough design notes
in
[ControlPlaneIPC.md](../Libraries/LibWeb/WebAudio/Engine/ControlPlaneIPC.md).

For timekeeping, the helper publishes a small shared "timing page" (current rendered frames,
underruns, suspend state, and a generation token). WebContent reads this to implement
`currentTime` and to resolve suspend/resume promises only after the render thread has
actually applied the transition:
- [SharedMemory timing page helpers](../Libraries/LibWeb/WebAudio/Engine/SharedMemory.h)

2) Data plane (high rate, shared memory)

For moving audio samples and blocks around without a storm of IPC, we use shared memory
plus atomics, with "notify fds" as wakeup hints:

- Overview: [DataPlaneTransport.md](../Libraries/LibWeb/WebAudio/Engine/DataPlaneTransport.md)
- Shared memory primitives: [StreamTransport.h](../Libraries/LibWeb/WebAudio/Engine/StreamTransport.h)
- Ring helpers: [StreamTransportRing.h](../Libraries/LibWeb/WebAudio/Engine/StreamTransportRing.h)
- Notify fd creation: [StreamTransportEventFD.h](../Libraries/LibWeb/WebAudio/Engine/StreamTransportEventFD.h)

This split is one of the ways we keep renderland clean: the render thread mostly just
reads/writes shared memory.

## AudioWorklet: first class, out-of-process

AudioWorklet is a big part of why we have a helper process.

On the page side, AudioWorklet is exposed by:
- [AudioWorklet](../Libraries/LibWeb/WebAudio/AudioWorklet.cpp)
- [AudioWorkletNode](../Libraries/LibWeb/WebAudio/AudioWorkletNode.cpp)

In WebAudioWorker, the realtime host that evaluates modules and runs processors is:
- [RealtimeAudioWorkletProcessorHost](../Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp)

Realtime AudioWorklet processing runs synchronously on the render thread, so process() completes within the current render quantum.

The message port wiring that connects node <-> processor out-of-process is implemented in
the worklet helper code:
- [MessagePortTransport](../Libraries/LibWeb/WebAudio/Worklet/MessagePortTransport.cpp)
- [Worklet port binding types](../Libraries/LibWeb/WebAudio/Worklet/WorkletPortBinding.h)

Notes about spec gaps and intended future work live here:
- [Worklet spec divergences](../Libraries/LibWeb/WebAudio/Worklet/SpecDivergences.md)

## A short detour: AudioServer and device playback

This is slightly out of scope for WebAudio itself, but it matters for the full system.

This WebAudio branch uses a dedicated AudioServer process to own the OS audio device. THis is
partly to allow WebContent to run with lower privileges, but also to avoid every WebContent tab
(and every helper) independently hammering the audio device, and instead have one place that:

- Opens and queries devices.
- Mixes audio from multiple producers.
- Manages target latency and stream lifecycle.

The initial abstraction here is
[AudioOutputDevice](../Services/AudioServer/AudioOutputDevice.cpp): it keeps a published
snapshot of active producer rings and mixes them in the device callback.
Each WebAudio session is just one producer writing into a shared ring buffer,
and AudioServer is responsible for combining those streams and handing the result to the
OS backend. Soon, AudioServer will enumerate input devices (mics) and establish /
publish those streams on demand.

## Debugging

There are a few opt-in environment variables intended for investigating scheduling,
render, and graph update issues.

Set `WEBAUDIO_LOG=1` to enable verbose WebAudio logging.
- More targeted logging options live in [Debug.h](../Libraries/LibWeb/WebAudio/Debug.h)
- This is not realtime-safe and will perturb timing.

Set `AUDIO_SERVER_LOG=1` to enable AudioServer logging.

In debug builds, WebAudio provides thread assertions that make it easier to spot incorrect
thread usage early:
- [WebAudio thread asserts](../Libraries/LibWeb/WebAudio/Debug.h)
