/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/WireCodec.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/PeriodicWave.h>

namespace Web::WebAudio::Render {

class RenderNode;
class GraphResourceResolver;

struct WEB_API OscillatorGraphNode {
    OscillatorType type { OscillatorType::Sine };

    // Base values for AudioParams.
    f32 frequency { 440.0f };
    f32 detune_cents { 0.0f };

    Optional<size_t> start_frame;
    Optional<size_t> stop_frame;

    Optional<PeriodicWaveCoefficients> periodic_wave;

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<OscillatorGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(OscillatorGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(OscillatorParamIndex::frequency, frequency, 0.0f, AK::NumericLimits<f32>::max());
        set_state(OscillatorParamIndex::detune, detune_cents, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(OscillatorParamIndex::frequency, frequency);
        update_intrinsic(OscillatorParamIndex::detune, detune_cents);
    }
};

}
