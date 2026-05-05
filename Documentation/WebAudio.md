# WebAudio in Ladybird: a practical architecture overview

This document explains how WebAudio works in this fork.

## Big picture

The Web Audio spec defines a control thread and a rendering thread:
- https://webaudio.github.io/web-audio-api/#control-thread
- https://webaudio.github.io/web-audio-api/#rendering-thread

In Ladybird terms:

Control thread: Main WebContent thread where JavaScript lives.
- Creates nodes, connects them, schedules starts/stops, edits parameters.
- Owns promises (suspend/resume/start rendering) and user-observable context and node state.

Render thread: Where audio is produced. Either an OfflineRenderThread or a WebAudioWorker process.
- Processes audio in fixed-size render quantums, currently 128 frames.
- Must be fast and predictable on tight deadlines (DSP, mixing, realtime work).

For live contexts, the render side is an isolated WebAudioWorker process. This is motivated by
AudioWorklet. The worklet code needs a place to run user scripts in the render pipeline that is
predictable and not constantly fighting the page's main thread.

### At a glance (process model)

```
                      +---------------------------------+
                      |  Ladybird process (the broker)  |
                      +----+-----------------------+----+
                           |                       |
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
  - [AudioContextRegistry](../Libraries/LibWeb/WebAudio/AudioContextRegistry.cpp)
  - [SessionRouter (owns the helper session + transports)](../Libraries/LibWeb/WebAudio/SessionRouter.cpp)

- Render process (WebAudioWorker):
  - [WebAudioRenderThread (shared render loop + mixing)](../Services/WebAudioWorker/WebAudioRenderThread.cpp)
  - [WebAudioSession (per-context state + graph swap + worklet host)](../Services/WebAudioWorker/WebAudioSession.cpp)

## Why a helper process?

In addition to the worklet concerns touched on above, realtime audio can be unforgiving.
If the audio callback misses its deadline, you get dropouts, crackles, and "why does this
sound broken" bugs that can be tough to crcak.

Keeping the render loop out of WebContent gives us a dedicated render thread with a stable
priority and a clear job. A side benefit is the process boundaries help us enforce
rules for what can run on the realtime render thread (and what absolutely must not).

Render-thread node implementations (DSP and processing) live in the RenderNodes directory:
- [RenderNodes overview](../Libraries/LibWebAudio/RenderNodes/README.md)
- Example: [GainRenderNode](../Libraries/LibWebAudio/RenderNodes/GainRenderNode.cpp)

### Realtime render thread rules

1. Do not allocate or free heap memory in steady state. Avoid growthy Vector operations, no GC.
2. Avoid locks and blocking. No mutexes, no condition variables, no waits.
3. Avoid syscalls with unbounded latency. No filesystem or network access, no non-debug logging.
4. Keep work bounded. Avoid recursion.

## The render graph: snapshots, compilation, and execution

From the page's point of view, WebAudio is a graph of nodes and connections.
From the render thread's point of view, it is a compact description that can be executed
quickly.

Ladybird bridges the gap between script adjacent nodes and audio production by snapshotting the
IDL-facing objects into a render description and then running that description on the render thread.

This snapshot is a "frozen" view of an AudioContext's nodes and connections with render-safe data:

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
- [GraphNodeTypes](../Libraries/LibWebAudio/GraphNodes/GraphNodeTypes.h)
- [GraphDescription](../Libraries/LibWebAudio/Engine/GraphDescription.h)
- [GraphNodes](../Libraries/LibWebAudio/GraphNodes/)

Compile and execute the node graph (render side):
- [GraphCompiler](../Libraries/LibWebAudio/Engine/GraphCompiler.cpp)
- [GraphExecutor](../Libraries/LibWebAudio/Engine/GraphExecutor.cpp)

# IPC and transports: control plane vs data plane

There are two kinds of traffic between WebContent and WebAudioWorker:

1) Control plane (low rate, structured messages)

Messages include:
- "Here is a new graph snapshot"
- Suspend / resume
- "Bind this ScriptProcessr transport"
- "Bind this worklet port"

See [ControlPlaneIPC.md](../Libraries/LibWeb/WebAudio/Engine/ControlPlaneIPC.md).

For timekeeping and context state, the helper publishes a small shared "timing page" (current
rendered frames, underruns, suspend state, and a generation token). WebContent reads this to
implement `currentTime` and to resolve suspend/resume promises only after the render thread has
actually applied the transition:

- [Timing page helpers](../Libraries/LibWebAudio/LibWebAudio.h)

2) Data plane (high rate, shared memory)

For moving audio samples and blocks around without a storm of IPC, we use shared memory
plus atomics, with "notify fds" as wakeup hints:

- Overview: [DataPlaneTransport.md](../Libraries/LibWeb/WebAudio/Engine/DataPlaneTransport.md)
- Shared memory primitives + transport descriptors: [LibWebAudio.h](../Libraries/LibWebAudio/LibWebAudio.h)
- SharedAudioBuffer: [SharedAudioBuffer.h](../Libraries/LibWebAudio/SharedAudioBuffer.h)
- SharedBufferStream: [SharedBufferStream.h](../Libraries/LibWebAudio/SharedBufferStream.h)

# AudioWorklet

AudioWorklet is one reason why realtime is rendered outside of WebContent because Ladybird's javascript
VM is a process singleton. This presents a problem when worklets must execute on the render thread, so we
bring up a VM for worklets in a dedicated process.

In WebAudioWorker, the realtime host that evaluates modules and runs processors is:
- [RealtimeAudioWorkletProcessorHost](../Libraries/LibWeb/WebAudio/Script/RealtimeAudioWorkletProcessorHost.cpp)

Notes about spec gaps and intended future work live here:
- [Worklet spec divergences](../Libraries/LibWeb/WebAudio/Script/SpecDivergences.md)

# AudioServer and device playback

This branch uses a dedicated AudioServer process that owns the OS audio device(s). (One day) tHis lets
WebContent run with lower privileges. It also keeps Ladybird from hitting the output device from multiple
places. Instead, there is one process to:

- Open and query devices.
- Mix audio from multiple producers.
- Manage stream lifecycle.
- Publish input device streams

# Debugging

Set `WEBAUDIO_LOG=255` to enable verbose WebAudio logging.
- More targeted logging options live in [Debug.h](../Libraries/LibWebAudio/Debug.h)
- Logging is not realtime-safe and will perturb timing.

In debug builds, WebAudio employs thread assertions to enforce render/control thread correctness.
You may notice that nearly all engine code paths begin with `ASSERT_CONTROL_THREAD` or `ASSERT_RENDER_THREAD`

# Further reading
- [Render nodes](../Libraries/LibWebAudio/RenderNodes/README.md)
- [Engine notes](../Libraries/LibWeb/WebAudio/Engine/README.md)

