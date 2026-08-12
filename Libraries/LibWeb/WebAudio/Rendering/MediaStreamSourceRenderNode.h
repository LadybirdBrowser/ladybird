/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
class MediaStreamSourceRenderNode final : public RenderNode {
public:
    MediaStreamSourceRenderNode(NodeID, size_t quantum_size);

    virtual void process(RenderGraph&, RenderContext const&) override;
    virtual void handle_message(NodeMessage const&) override;

private:
    RefPtr<Media::SpscAudioFrameRing> m_ring;
    u32 m_channel_count { 1 };
    Vector<float> m_interleaved_scratch;
};

}
