/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

// The render thread only touches the ring and pending flag; the control thread owns the drain
// callback so its captured GC roots never cross threads.
struct MediaStreamDestinationSharedState final : public AtomicRefCounted<MediaStreamDestinationSharedState> {
    explicit MediaStreamDestinationSharedState(NonnullRefPtr<Media::SpscAudioFrameRing> ring)
        : ring(move(ring))
    {
    }

    NonnullRefPtr<Media::SpscAudioFrameRing> ring;

    // Set by the rendering thread when it schedules a drain on the control thread and cleared
    // by the control thread when the drain runs, coalescing the notifications down to one
    // wakeup per rendered burst.
    Atomic<bool> drain_pending { false };

    // Control thread only: drains the ring and forwards the frames to the node's track. Set
    // when the node is created and cleared in its finalizer, which also makes any drain still
    // queued by the rendering thread a no-op.
    Function<void()> drain;
};

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
class MediaStreamDestinationRenderNode final : public RenderNode {
public:
    MediaStreamDestinationRenderNode(NodeID, size_t quantum_size, size_t channel_count, NonnullRefPtr<MediaStreamDestinationSharedState>, Core::EventLoop& control_thread_event_loop);

    virtual void process(RenderGraph&, RenderContext const&) override;

private:
    NonnullRefPtr<MediaStreamDestinationSharedState> m_shared_state;
    Core::EventLoop& m_control_thread_event_loop;
    AudioBus m_mix_bus;
    Vector<float> m_interleaved_scratch;
};

}
