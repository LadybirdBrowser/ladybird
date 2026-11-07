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
#include <LibAudio/SpscAudioFrameRing.h>
#include <LibCore/Forward.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

struct MediaStreamDestinationSharedState final : public AtomicRefCounted<MediaStreamDestinationSharedState> {
    explicit MediaStreamDestinationSharedState(NonnullRefPtr<Audio::SpscAudioFrameRing> ring)
        : ring(move(ring))
    {
    }

    NonnullRefPtr<Audio::SpscAudioFrameRing> ring;

    // Only the control thread invokes or clears this callback.
    Function<void()> drain;
};

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
class MediaStreamDestinationRenderNode final : public RenderNode {
public:
    MediaStreamDestinationRenderNode(NodeID, size_t quantum_size, size_t channel_count, NonnullRefPtr<MediaStreamDestinationSharedState>, Core::EventLoop& control_thread_event_loop);
};

}
