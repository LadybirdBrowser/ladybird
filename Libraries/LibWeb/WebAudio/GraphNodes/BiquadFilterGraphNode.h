/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResourceResolver;

struct WEB_API BiquadFilterGraphNode {
    BiquadFilterType type { BiquadFilterType::Lowpass };

    // Base values for AudioParams.
    f32 frequency_hz { 350.0f };
    f32 detune_cents { 0.0f };
    f32 q { 1.0f };
    f32 gain_db { 0.0f };

    size_t channel_count { 2 };
    ChannelCountMode channel_count_mode { ChannelCountMode::Max };
    ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<BiquadFilterGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(BiquadFilterGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(f32 sample_rate, SetState&& set_state) const
    {
        f32 const nyquist = static_cast<f32>(sample_rate * 0.5f);
        set_state(BiquadFilterParamIndex::frequency, frequency_hz, 0.0f, nyquist);
        set_state(BiquadFilterParamIndex::detune, detune_cents, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
        set_state(BiquadFilterParamIndex::q, q, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
        set_state(BiquadFilterParamIndex::gain, gain_db, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(BiquadFilterParamIndex::frequency, frequency_hz);
        update_intrinsic(BiquadFilterParamIndex::detune, detune_cents);
        update_intrinsic(BiquadFilterParamIndex::q, q);
        update_intrinsic(BiquadFilterParamIndex::gain, gain_db);
    }
};

}
