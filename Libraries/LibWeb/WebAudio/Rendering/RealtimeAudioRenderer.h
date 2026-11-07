/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/SeqLock.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibAudio/PlaybackStream.h>
#include <LibCore/Forward.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Rendering {

// Renders an AudioContext's graph in real time, pulled by the audio output device: the playback stream's data request
// callback runs on the platform's audio thread and renders just enough render quanta to fill the device's buffer.
// Progress is reported back to the control thread by posting events to the main thread's event loop.
// https://webaudio.github.io/web-audio-api/#AudioContext
struct RealtimeAudioRendererCallbacks;

class WEB_API RealtimeAudioRenderer final : public AtomicRefCounted<RealtimeAudioRenderer> {
public:
    // Device-buffer latency target. Public because AudioWorkletPipe derives its lookahead from the
    // renderer's burst size, which this constant determines.
    static constexpr u32 TARGET_LATENCY_MS = 100;

    static NonnullRefPtr<RealtimeAudioRenderer> create(NonnullRefPtr<ControlMessageQueue>, NodeID destination_node_id, float sample_rate, size_t quantum_size, NonnullRefPtr<Media::AudioOutputFactory>);
    ~RealtimeAudioRenderer();

    // The callback below is set on the control thread before rendering starts and is always invoked on the control
    // thread. It is cleared when the context closes to break the reference cycle with the context.
    void set_on_sources_ended(Function<void(Vector<NodeID> const&)>);
    void clear_callbacks();

    void start_rendering();
    void resume();
    void suspend();
    void stop();

    u64 frames_rendered() const;

    // The amount of audio the output device has played so far, in seconds.
    double output_time_played() const;

private:
    RealtimeAudioRenderer(NonnullRefPtr<ControlMessageQueue>, NodeID destination_node_id, float sample_rate, size_t quantum_size, NonnullRefPtr<Media::AudioOutputFactory>);

    void prepare_to_start_rendering();
    void set_playback_stream(NonnullRefPtr<Audio::PlaybackStream>);
    ReadonlySpan<float> fill_output_buffer(Span<float>, MonotonicTime buffer_starts_playing_at);

    NonnullRefPtr<RealtimeAudioRendererCallbacks> m_callbacks;
    NonnullRefPtr<Media::AudioOutputFactory> m_audio_output_factory;
    RefPtr<Audio::PlaybackStream> m_playback_stream;
    void const* m_renderer { nullptr };
};

}
