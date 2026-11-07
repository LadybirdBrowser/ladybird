/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/Time.h>
#include <LibAudio/PlaybackStream.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio {

static constexpr u32 TARGET_LATENCY_MS = 100;

extern "C" RustFFI::FfiSharedCppObject web_audio_output_factory_create(DOM::EventTarget*);
extern "C" double web_audio_realtime_context_performance_now(DOM::EventTarget*);

RustFFI::FfiSharedCppObject web_audio_output_factory_create(DOM::EventTarget* global)
{
    auto factory = as<HTML::Window>(*global).page().client().create_audio_output_factory();
    return { &factory.leak_ref(), [](void* factory) { static_cast<Media::AudioOutputFactory*>(factory)->ref(); }, [](void* factory) { static_cast<Media::AudioOutputFactory*>(factory)->unref(); } };
}

// Keeps the renderer alive for the callbacks of its stream.
class RendererReference final : public AtomicRefCounted<RendererReference> {
public:
    explicit RendererReference(void const* renderer)
        : m_renderer(renderer)
    {
        RustFFI::web_audio_realtime_renderer_retain(m_renderer);
    }

    ~RendererReference() { RustFFI::web_audio_realtime_renderer_release(m_renderer); }

    void const* renderer() const { return m_renderer; }

private:
    void const* m_renderer { nullptr };
};

void RustFFI::web_audio_output_open(void* factory, void const* renderer)
{
    auto reference = adopt_ref(*new RendererReference(renderer));
    auto promise = static_cast<Media::AudioOutputFactory*>(factory)->open_output(Audio::OutputState::Suspended, TARGET_LATENCY_MS, [reference](Span<float> buffer, MonotonicTime buffer_starts_playing_at) {
        return buffer.trim(RustFFI::web_audio_realtime_renderer_fill(reference->renderer(), buffer, buffer_starts_playing_at.nanoseconds()));
    });
    promise->when_resolved([reference](NonnullRefPtr<Audio::PlaybackStream>& stream) {
        stream->on_output_lost = [reference] { RustFFI::web_audio_realtime_renderer_output_lost(reference->renderer()); };
        auto specification = stream->sample_specification();
        RustFFI::FfiSharedCppObject shared_stream { stream.ptr(), [](void* stream) { static_cast<Audio::PlaybackStream*>(stream)->ref(); }, [](void* stream) { static_cast<Audio::PlaybackStream*>(stream)->unref(); } };
        RustFFI::web_audio_realtime_renderer_output_opened(reference->renderer(), shared_stream, specification.channel_count(), specification.sample_rate());
    });
}

void RustFFI::web_audio_output_resume(void* stream)
{
    static_cast<Audio::PlaybackStream*>(stream)->resume()->when_rejected([](auto&&) { });
}
void RustFFI::web_audio_output_drain_and_suspend(void* stream)
{
    static_cast<Audio::PlaybackStream*>(stream)->drain_buffer_and_suspend()->when_rejected([](auto&&) { });
}
void RustFFI::web_audio_output_discard_and_suspend(void* stream)
{
    static_cast<Audio::PlaybackStream*>(stream)->discard_buffer_and_suspend()->when_rejected([](auto&&) { });
}
i64 RustFFI::web_audio_monotonic_now_nanoseconds() { return MonotonicTime::now().nanoseconds(); }
double web_audio_realtime_context_performance_now(DOM::EventTarget* global) { return as<HTML::Window>(*global).performance()->now(); }

}
