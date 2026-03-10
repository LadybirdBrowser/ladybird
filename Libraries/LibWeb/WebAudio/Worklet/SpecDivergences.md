# WebAudio Worklet spec divergences (gpt)

This file tracks known or suspected gaps between our Worklet implementation and the Web Audio specification.

Spec: https://webaudio.github.io/web-audio-api/#AudioWorklet

TLDR - most of this will go away once we have a Worklet base that implements module loading.

## Update 2026-02-09
- OfflineAudioContext renderSizeHint is now supported for per-context render quantum sizing; no worklet-specific divergence noted.

## Update 2026-02-10
- AudioPlaybackStats now reads timing page counters; no worklet-specific divergence noted.

## Worklet module loading: imported module graphs are not supported
- Spec expectation: AudioWorklet.addModule() loads and evaluates the module and its dependency graph (static imports), with failures surfaced via the returned promise.
- Current behavior: The realtime host parses and evaluates the provided module source text, but does not load requested modules; imported modules are not supported.
- Reason: We don't have a module loader / fetch pipeline wired up yet. No Worklet base and nothing in the host.

## Time origin: AudioWorkletEnvironmentSettingsObject uses a hardcoded time origin
- Spec expectation: Worklet global scope time origin should be derived from the environment settings object and align with the relevant time origin used by Performance/HR-Time for the global.
- Current behavior: The worklet host sets `outside_settings.time_origin` to 0.
- Reason: WebAudioWorker has no browsing context; the host constructs a minimal Page/PageClient and uses a fixed value instead of plumbing an origin timestamp.

## Event loop integration: worklet host drives the HTML event loop manually
- Spec expectation: AudioWorkletGlobalScope tasks and microtasks run under the standard event loop model for the agent, without relying on ad-hoc pumping.
- Current behavior: The worklet host explicitly pumps Core::EventLoop and manually calls HTML event loop `process()` in a bounded loop.
- Reason: The AudioWorklet host thread is not a normal Window/Worker thread that naturally runs an HTML event loop; MessagePort delivery enqueues global tasks that must be drained.

## Processor construction: instance creation is gated on MessagePort transport availability
- Spec expectation: Processor construction and MessagePort semantics should not depend on transport timing; messages posted from the processor constructor should be queued and eventually delivered.
- Current behavior: The realtime host delays constructing the processor instance until the port transport is attached; otherwise, constructor-time messages are known to get dropped.
- Reason: Without an attached transport, there is nowhere to send queued messages; rather than buffering, construction is postponed.
