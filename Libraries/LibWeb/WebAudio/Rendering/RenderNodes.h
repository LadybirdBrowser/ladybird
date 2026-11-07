/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#include <LibWeb/WebAudio/Rendering/RenderNode.h>
namespace Web::WebAudio::Rendering {

class DestinationRenderNode final : public RenderNode {
public:
    DestinationRenderNode(NodeID, size_t channel_count, size_t quantum_size);
};
class GainRenderNode final : public RenderNode {
public:
    GainRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> gain);
};
class ConstantSourceRenderNode final : public RenderNode {
public:
    ConstantSourceRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> offset);
};
class OscillatorRenderNode final : public RenderNode {
public:
    OscillatorRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune);
};
class AudioBufferSourceRenderNode final : public RenderNode {
public:
    AudioBufferSourceRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> playback_rate, NonnullRefPtr<RenderAudioParam> detune);
};
class DelayRenderNode final : public RenderNode {
public:
    DelayRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> delay_time, double max_delay_time);
};
class StereoPannerRenderNode final : public RenderNode {
public:
    StereoPannerRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> pan);
};
class ChannelSplitterRenderNode final : public RenderNode {
public:
    ChannelSplitterRenderNode(NodeID, size_t quantum_size, size_t number_of_outputs);
};
class ChannelMergerRenderNode final : public RenderNode {
public:
    ChannelMergerRenderNode(NodeID, size_t quantum_size, size_t number_of_inputs);
};
class BiquadFilterRenderNode final : public RenderNode {
public:
    BiquadFilterRenderNode(NodeID, size_t quantum_size, NonnullRefPtr<RenderAudioParam> frequency, NonnullRefPtr<RenderAudioParam> detune, NonnullRefPtr<RenderAudioParam> q, NonnullRefPtr<RenderAudioParam> gain);
};
class IIRFilterRenderNode final : public RenderNode {
public:
    IIRFilterRenderNode(NodeID, size_t quantum_size, Vector<double> feedforward, Vector<double> feedback);
};
class WaveShaperRenderNode final : public RenderNode {
public:
    WaveShaperRenderNode(NodeID, size_t quantum_size);
};
class ConvolverRenderNode final : public RenderNode {
public:
    ConvolverRenderNode(NodeID, size_t quantum_size);
};
class PannerRenderNode final : public RenderNode {
public:
    struct Params {
        NonnullRefPtr<RenderAudioParam> position_x;
        NonnullRefPtr<RenderAudioParam> position_y;
        NonnullRefPtr<RenderAudioParam> position_z;
        NonnullRefPtr<RenderAudioParam> orientation_x;
        NonnullRefPtr<RenderAudioParam> orientation_y;
        NonnullRefPtr<RenderAudioParam> orientation_z;
    };
    struct ListenerParams {
        NonnullRefPtr<RenderAudioParam> position_x;
        NonnullRefPtr<RenderAudioParam> position_y;
        NonnullRefPtr<RenderAudioParam> position_z;
        NonnullRefPtr<RenderAudioParam> forward_x;
        NonnullRefPtr<RenderAudioParam> forward_y;
        NonnullRefPtr<RenderAudioParam> forward_z;
        NonnullRefPtr<RenderAudioParam> up_x;
        NonnullRefPtr<RenderAudioParam> up_y;
        NonnullRefPtr<RenderAudioParam> up_z;
    };
    PannerRenderNode(NodeID, size_t quantum_size, Params, ListenerParams);
};
class PassthroughRenderNode final : public RenderNode {
public:
    PassthroughRenderNode(NodeID, size_t quantum_size);
};

}
