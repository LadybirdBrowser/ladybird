# Media Pipeline

## Overview

LibMedia turns demuxed media data into decoded audio and video, then feeds it to sinks that are
driven by playback time. The pipeline is pull-based: downstream nodes ask upstream nodes whether
data is available, then pull data when the reported status allows it.

The current pipeline is made of three categories of nodes:

- Producers materialize raw audio/video data.
- Sinks consume that raw audio/video data.
- Processors are both producers and sinks, pulling data from an input producer to transform it
  before a downstream sink makes use of it.

Audio and video use separate producer interfaces because the data they carry is different, but their
control flow is intentionally similar.

## Producer Interface

Producers expose three core operations:

- `peek()` reports the current status and, when data is available, exposes the head of the
  producer's output without removing it.
- `consume()` removes the head that a prior `peek()` exposed.
- `set_wake_handler(...)` is used by the downstream node to wake itself when a previously-observed
  status would result in it going to sleep.

Downstream nodes `peek()` when they need new data, then either read the head and `consume()` it or
go to sleep. Peeking without consuming lets a node decide whether to take the head, rather than
being forced to accept data it may have no room for.

## PipelineStatus

`PipelineStatus` is the shared status language between nodes:

- `Pending`: Data is not available yet, but may become available later.
- `HaveData`: A peek exposes data.
- `Blocked`: Data is not available yet, i.e. we have run out of buffered data from over the network.
- `Suspended`: The upstream node has released its decoding resources and will not produce data until
  it receives a seek.
- `EndOfStream`: We have reached the end of the input data.
- `Error`: The upstream node encountered an error and cannot continue without a seek.

`HaveData` carries data. All other statuses require handling by downstream code.

`Pending`, `Blocked`, `Suspended`, `EndOfStream` and `Error` are waiting statuses. A downstream node
that normally polls for upstream input seeing these should sleep until its wake handler fires.

`EndOfStream` and `Error` are terminal statuses. They resolve seeks even though they are still
"waiting" statuses for helper APIs, because no more data is required to decide the seek has
completed.

## Wake Handlers

Wake handlers do not carry state. A wake means the downstream node should `peek()` again.

Nodes should dispatch a wake when the downstream node may be waiting, and the status has changed to
one that resolves a seek: `HaveData`, `EndOfStream`, or `Error`.

Some nodes (i.e. AudioMixer) may also wake when moving away from a previously dispatched
`EndOfStream`, so stale EOS state can be recovered after a seek or after new input becomes
available.

## Seeking

Seeking is initiated downstream and forwarded upstream. For downstream nodes, a seek completes when
the upstream node reports a status that resolves the new position. Usually that means `HaveData`,
but `EndOfStream` or `Error` can also complete the wait.

Sinks report pipeline status changes to their users. A user such as the playback manager can
interpret a seek-resolving status as seek completion.

If a node forwards a seek upstream, it can clear or invalidate its cached data, since the upstream
node is required to emit data at or before the target timestamp.

However, if a node uses its cached data to resolve seeks, it must also resolve seeks past the last
frame in that cached data if its last known status is `EndOfStream`. This ensures that a seek past
EOS will not invalidate in-flight frames by bumping a seek generation.

## Decoded Data Producers

Decoded data producers run demux and decode video/audio data into queues in order to absorb variance
in processing for each individual coded frame.

When a downstream `consume()` removes an item from the queue, the producer wakes its decoder thread
so it can refill the queue. When the queue is full and no downstream activity occurs for the idle
timeout, the producer auto-suspends: it drops its decoder, its queue, and any frame pool buffers it
can free, then wakes the consumer with a `Suspended` status.

`peek()` and `consume()` defer the idle timeout, but they never resume a suspended producer. When a
downstream node needs new data, it must explicitly `seek()` the suspended node to resume it, so that
the pipeline may insert pre-roll data at any stage where it is needed (i.e. for audio stretching).

## Playback Manager

`PlaybackManager` creates, modifies and drives the end-to-end playback pipeline. When media data is
received, it will spawn a thread to determine what tracks are present, and connect the necessary
nodes for playback to begin.

Behavior is defined through state handlers derived from `PlaybackStateHandler`, with each overriding
methods for various functions and signals to determine when to transition to another state. Upon
entering or exiting, a state handler will drive changes to the pipeline, like pausing/resuming or
seeking.

The media time is derived from the audio sink when audio is used, otherwise `GenericTimeProvider`
calculates time from the monotonic clock.

## Audio Pipeline

The usual audio path is:

```text
DecodedAudioProducer
    -> AudioMixer
    -> AudioTimeStretchProcessor
    -> AudioPlaybackSink
    -> PlaybackStream
```

`AudioPlaybackSink` owns a small queue of `AudioBlock`s pulled from upstream, which it fills on a
separate thread to avoid blocking the `PlaybackStream` data callback. When its input reaches
`EndOfStream`, it continues producing silence to allow the time to advance in case there is video
data still available.

`AudioMixer` sequences/mixes audio from multiple inputs into a single output. It emits silence for
gaps, and pauses output when any input indicates data is not ready yet. When all inputs are at
`EndOfStream`, the output becomes `EndOfStream` as well.

`AudioTimeStretchProcessor` stretches audio without affecting pitch.

## Video Pipeline

The usual video path is:

```text
DecodedVideoProducer -> DisplayingVideoSink
```

`DisplayingVideoSink` keeps the current frame and the next frame. It advances when playback time
passes the current frame's display interval.

Video seeks are comparatively expensive, so the sink can satisfy some seeks locally when the
requested timestamp is already covered by the current or next frame interval. This avoids
unnecessary upstream seeks during scrubbing. The same optimization is less important for audio
because it decodes exponentially faster.

## Threading

The media pipeline crosses several threads:

- The main thread coordinates playback state and serializes cross-thread communication through its
  event loop
- Decoded data producers manage decoding threads to amortize the cost to decode each frame
- The audio sink runs a processing thread to amortize the cost of mixing/stretching each audio block
- The playback stream data callback is invoked from an (often system-managed) thread

Data should only be consumed via `peek()` and `consume()` from a single thread, to allow producers
to use lock-free structures internally. Control methods like `seek()` should similarly only initiate
from a single thread, or be otherwise serialized.

Avoid synchronous callback dispatch while holding locks unless the callee is known not to reenter
the same pipeline. Deferred dispatch is often used to keep lock ordering simple.

Wake handlers must be removed before disconnecting or destroying a node. An upstream node must never
invoke a handler after downstream has cleared it.
