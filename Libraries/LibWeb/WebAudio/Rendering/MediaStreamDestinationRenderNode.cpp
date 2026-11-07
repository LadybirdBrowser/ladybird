/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/MediaStreamDestinationRenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

struct DestinationCallbackService : AtomicRefCounted<DestinationCallbackService> {
    DestinationCallbackService(NonnullRefPtr<MediaStreamDestinationSharedState> state, Core::EventLoop& loop)
        : shared_state(move(state))
        , event_loop(loop)
    {
    }
    NonnullRefPtr<MediaStreamDestinationSharedState> shared_state;
    Core::EventLoop& event_loop;
};

static void* create_render_node(NodeID node_id, size_t quantum_size, size_t channel_count, NonnullRefPtr<MediaStreamDestinationSharedState> shared_state, Core::EventLoop& event_loop)
{
    auto service = adopt_ref(*new DestinationCallbackService(shared_state, event_loop));
    RustFFI::PipeCallbacks callbacks {
        { service.ptr(), [](void* service) { static_cast<DestinationCallbackService*>(service)->ref(); }, [](void* service) { static_cast<DestinationCallbackService*>(service)->unref(); } },
        [](void* service, void* task, void (*invoke)(void*)) {
            static_cast<DestinationCallbackService*>(service)->event_loop.deferred_invoke([task, invoke] { invoke(task); });
        },
        [](void* service) {
            auto& shared_state = *static_cast<DestinationCallbackService*>(service)->shared_state;
            if (shared_state.drain)
                shared_state.drain();
        },
        [](void*, size_t, u64) { return false; },
    };
    return RustFFI::web_audio_node_create_media_stream_destination(node_id.value(), quantum_size, channel_count,
        { shared_state->ring.ptr(), [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->ref(); }, [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->unref(); } },
        {
            [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->channel_count(); },
            [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_available(); },
            [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_free(); },
            [](void* ring, Span<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_pop(samples); },
            [](void* ring, ReadonlySpan<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_push(samples); },
        },
        callbacks);
}

MediaStreamDestinationRenderNode::MediaStreamDestinationRenderNode(NodeID node_id, size_t quantum_size, size_t channel_count, NonnullRefPtr<MediaStreamDestinationSharedState> shared_state, Core::EventLoop& control_thread_event_loop)
    : RenderNode(create_render_node(node_id, quantum_size, channel_count, move(shared_state), control_thread_event_loop))
{
}

}
