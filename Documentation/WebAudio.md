# WebAudio in Ladybird: a practical architecture overview

This document explains how WebAudio works in this fork.

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
                      |  AudioServer (device output)    |
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
  - [EngineController (process-global coordinator)](../Libraries/LibWeb/WebAudio/EngineController.cpp)
  - [WebAudioClientRegistry (session and timing coordinator)](../Libraries/LibWeb/WebAudio/Engine/WebAudioClientRegistry.cpp)
  - [WebAudioWorkerSession (owns the helper session + transports)](../Libraries/LibWeb/WebAudio/Engine/WebAudioWorkerSession.cpp)

- Render process (WebAudioWorker):
  - [WebAudioRenderThread (shared render loop + mixing)](../Services/WebAudioWorker/WebAudioRenderThread.cpp)
  - [WebAudioSession (per-context state + graph swap + worklet host)](../Services/WebAudioWorker/WebAudioSession.cpp)
  - [WebAudioWorker control connection](../Services/WebAudioWorker/WebAudioWorkerConnection.cpp)
  - [WebAudioServer control-plane IPC surface](../Services/WebAudioWorker/WebAudioServer.ipc)

- Audio device process (AudioServer):
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

Render-thread node implementations (DSP and processing) live in the RenderNodes directory:
- [RenderNodes overview](../Libraries/LibWeb/WebAudio/RenderNodes/README.md)
- Example: [GainRenderNode](../Libraries/LibWeb/WebAudio/RenderNodes/GainRenderNode.cpp)

### Dos and donts for the render thread

1. Do not allocate or free heap memory in steady state. Avoid growthy Vector operations, no GC.
2. Avoid locks and blocking. No mutexes, no condition variables, no waits.
3. Avoid syscalls with unbounded latency. No filesystem or network access, no non-debug logging.
4. Keep work bounded. Avoid recursion.

## The render graph: snapshots, compilation, and execution

From the page's point of view, WebAudio is a graph of nodes and connections.
From the render thread's point of view, it is a compact description that can be executed
quickly.

Ladybird bridges that gap by snapshotting the IDL-facing objects into a render description
and then running that description on the render thread.

### Why flat snapshots?

The snapshot is a "frozen" view of an AudioContext's nodes and connections with render-safe data:

- Plain numbers and small structs.
- Stable IDs instead of pointers.
- Handles to external resources (like decoded PCM) rather than owning heavyweight objects.

That way the render thread process (WebAudioWorker) does not need to touch DOM objects, GC things, or
page state.

### Important pieces:

Register an AudioContext:
- [EngineController](../Libraries/LibWeb/WebAudio/EngineController.cpp)

Build a graph of flat, serializable node descriptions (control thread):
- [GraphBuilder](../Libraries/LibWeb/WebAudio/GraphBuilder.cpp)
- [GraphNodeTypes](../Libraries/LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h)
- [GraphDescription](../Libraries/LibWeb/WebAudio/Engine/GraphDescription.h)
- [GraphNodes](../Libraries/LibWeb/WebAudio/GraphNodes/)

Compile and execute the node graph (render side):
- [GraphCompiler](../Libraries/LibWeb/WebAudio/Engine/GraphCompiler.cpp)
- [GraphExecutor](../Libraries/LibWeb/WebAudio/Engine/GraphExecutor.cpp)

# IPC and transports: control plane vs data plane

There are two kinds of traffic between WebContent and WebAudioWorker:

1) Control plane (low rate, structured messages)

Messages include:
- "Here is a new graph snapshot"
- Suspend / resume
- "Bind this ScriptProcessr transport"
- "Bind this worklet port"

See [ControlPlaneIPC.md](../Libraries/LibWeb/WebAudio/Engine/ControlPlaneIPC.md).

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

# AudioWorklet: first class, out-of-process

AudioWorklet is a big part of why realtime is rendered outside of WebContent. It was motivated by
Ladybird's javascript VM being a process singleton (at present). This presents a problem when worklets
must execute on the render thread, so we bring up a VM for worklets in a dedicated process.

On the page side, AudioWorklet is exposed by:
- [AudioWorklet](../Libraries/LibWeb/WebAudio/AudioWorklet.cpp)
- [AudioWorkletNode](../Libraries/LibWeb/WebAudio/AudioWorkletNode.cpp)

In WebAudioWorker, the realtime host that evaluates modules and runs processors is:
- [RealtimeAudioWorkletProcessorHost](../Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp)

The message port wiring that connects node <-> processor out-of-process is implemented in
the worklet helper code:
- [MessagePortTransport](../Libraries/LibWeb/WebAudio/Worklet/MessagePortTransport.cpp)
- [Worklet port binding types](../Libraries/LibWeb/WebAudio/Worklet/WorkletPortBinding.h)

Notes about spec gaps and intended future work live here:
- [Worklet spec divergences](../Libraries/LibWeb/WebAudio/Worklet/SpecDivergences.md)

# AudioServer and device playback

This branch uses a dedicated AudioServer process that owns the OS audio device(s). THis lets
WebContent run with lower privileges and keeps Ladybird from hitting the output device from multiple
processes. Instead, there is one process to:

- Open and query devices.
- Mix audio from multiple producers.
- Manage stream lifecycle.
- (Coming soon) Publish input device streams

[AudioOutputDevice](../Services/AudioServer/AudioOutputDevice.cpp) keeps a published
snapshot of active producer rings and mixes them in the device callback.
Each WebAudio session is just one producer writing into its own shared ring buffer,
and AudioServer is responsible for combining those streams and handing the result to the
OS backend.

# Debugging

There are a few opt-in environment variables intended for investigating scheduling,
render, and graph update issues.

Set `WEBAUDIO_LOG=1` to enable verbose WebAudio logging.
- More targeted logging options live in [Debug.h](../Libraries/LibWeb/WebAudio/Debug.h)
- Logging is not realtime-safe and will perturb timing.

In debug builds, WebAudio employs thread assertions to enforce render/control thread correctness.
Nearly all engine code paths begin with `ASSERT_CONTROL_THREAD` or `ASSERT_RENDER_THREAD`

# Further reading
- [Render nodes](../Libraries/LibWeb/WebAudio/RenderNodes/README.md)
- [Engine overview](../Libraries/LibWeb/WebAudio/Engine/README.md)
- [Control-plane IPC](../Libraries/LibWeb/WebAudio/Engine/ControlPlaneIPC.md)
- [Data-plane transports](../Libraries/LibWeb/WebAudio/Engine/DataPlaneTransport.md)

