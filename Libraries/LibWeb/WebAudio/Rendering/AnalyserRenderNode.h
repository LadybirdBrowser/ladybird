/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibAudio/SpscAudioFrameRing.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

// https://webaudio.github.io/web-audio-api/#AnalyserNode
class AnalyserRenderNode final : public RenderNode {
public:
    AnalyserRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<Audio::SpscAudioFrameRing> time_domain_ring);
};

}
