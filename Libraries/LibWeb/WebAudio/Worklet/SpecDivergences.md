# WebAudio Worklet spec divergences (gpt)

This file tracks known or suspected gaps between our Worklet implementation and the Web Audio specification.

Scope:
- AudioWorklet and AudioWorkletGlobalScope behavior when hosted in WebAudioWorker / AudioServer.

Primary spec entry point:
- AudioWorkletGlobalScope: https://webaudio.github.io/web-audio-api/#AudioWorkletGlobalScope


## Divergences

### Worklet module loading: imported module graphs are not supported
- Spec expectation: AudioWorklet.addModule() loads and evaluates the module and its dependency graph (static imports), with failures surfaced via the returned promise.
- Current behavior: The realtime host parses and evaluates the provided module source text, but does not load requested modules; imported modules are not supported.
- Reason: The worklet host does not have a module loader / fetch pipeline wired up, and `load_requested_modules()` is invoked with a null loader.
- Next steps:
  - Provide a module loader for the worklet realm that can resolve and load static imports.
  - Define the policy surface (URL resolution, origin/CSP, and error propagation back to the main thread addModule promise).
  - Add a focused test that uses a static import from a sibling module.
- Code pointers:
  - Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp (module evaluation loops in thread_main)

### Worklet module loading: addModule failure reporting is not plumbed through
- Spec expectation: AudioWorklet.addModule() returns a promise that rejects when parse/link/evaluate fails.
- Current behavior: Module parse/link/evaluate failures are ignored (best-effort) and only optionally logged; the main-thread API does not receive a structured failure for the rejected promise.
- Reason: The realtime host runs out-of-process and currently treats module evaluation as opportunistic; there is no explicit failure channel back to the main thread.
- Next steps:
  - Define an IPC message for module evaluation success/failure and wire it to the addModule promise resolution/rejection.
  - Ensure the failure preserves a useful error message (URL + syntax/exception text).
  - Add a test that intentionally throws or has a syntax error in the processor module.
- Code pointers:
  - Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp (module evaluation loops in thread_main)

### Time origin: AudioWorkletEnvironmentSettingsObject uses a hardcoded time origin
- Spec expectation: Worklet global scope time origin should be derived from the environment settings object and align with the relevant time origin used by Performance/HR-Time for the global.
- Current behavior: The worklet host sets `outside_settings.time_origin` to 0.
- Reason: WebAudioWorker has no browsing context; the host constructs a minimal Page/PageClient and uses a fixed value instead of plumbing an origin timestamp.
- Next steps:
  - Decide what the time origin should be for WebAudioWorker (AudioContext creation time is a likely candidate).
  - Plumb that time origin from the main thread into the realtime worklet host.
  - Add a test for `currentTime` / `performance.timeOrigin` coherence in AudioWorkletGlobalScope, if applicable.
- Code pointers:
  - Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp (SerializedEnvironmentSettingsObject outside_settings initialization)

### Event loop integration: worklet host drives the HTML event loop manually
- Spec expectation: AudioWorkletGlobalScope tasks and microtasks run under the standard event loop model for the agent, without relying on ad-hoc pumping.
- Current behavior: The worklet host explicitly pumps Core::EventLoop and manually calls HTML event loop `process()` in a bounded loop.
- Reason: The AudioWorklet host thread is not a normal Window/Worker thread that naturally runs an HTML event loop; MessagePort delivery enqueues global tasks that must be drained.
- Next steps:
  - Revisit the event loop ownership model for the worklet host thread (single event loop, or well-defined ordering between loops).
  - Ensure task ordering and microtask checkpoints match expectations (especially around message delivery and processor construction).
  - Avoid busy-looping: define a principled waiting strategy (sleep/notify) when idle.
- Code pointers:
  - Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp (thread_main, core_event_loop.pump + html_event_loop.process)

### Processor construction: instance creation is gated on MessagePort transport availability
- Spec expectation: Processor construction and MessagePort semantics should not depend on transport timing; messages posted from the processor constructor should be queued and eventually delivered.
- Current behavior: The realtime host delays constructing the processor instance until the port transport is attached; otherwise, constructor-time messages are known to get dropped.
- Reason: Without an attached transport, there is nowhere to send queued messages; rather than buffering, construction is postponed.
- Next steps:
  - Add buffering in the MessagePort transport layer (or in the binding) so that messages posted before transport attachment are preserved.
  - Remove the construction gating once buffering is correct.
  - Add a test where the processor constructor posts a message and the node side reliably receives it.
- Code pointers:
  - Libraries/LibWeb/WebAudio/Worklet/RealtimeAudioWorkletProcessorHost.cpp (try_construct_missing_instances, ports_with_transport requirement)

## Conformance-sensitive behavior (verified)

### MessagePort: adding a message event listener implicitly starts the port
- Spec expectation: Adding a 'message' event listener on a MessagePort is treated as if MessagePort.start() was called.
- Current behavior: EventTarget::add_an_event_listener calls MessagePort::start() when the listener type is 'message'.
- Reason: This is required by the HTML spec and is relied on by WPTs that use addEventListener('message', ...).
- Next steps:
  - Keep this behavior intact when refactoring EventTarget/MessagePort.
  - If a WPT fails around message delivery, confirm that the port is enabled and that the event loop is draining tasks.
- Code pointers:
  - Libraries/LibWeb/DOM/EventTarget.cpp (EventTarget::add_an_event_listener)
