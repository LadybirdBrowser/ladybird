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

struct WEB_API ConstantSourceGraphNode {
    Optional<size_t> start_frame;
    Optional<size_t> stop_frame;

    // Base value for the offset AudioParam.
    f32 offset { 1.0f };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<ConstantSourceGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(ConstantSourceGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(ConstantSourceParamIndex::offset, offset, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(ConstantSourceParamIndex::offset, offset);
    }
};

}
