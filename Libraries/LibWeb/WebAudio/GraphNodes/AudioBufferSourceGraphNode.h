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

struct WEB_API AudioBufferSourceGraphNode {
    Optional<size_t> start_frame;
    Optional<size_t> stop_frame;

    Optional<f64> start_time_in_context_frames;

    // Base values for AudioParams.
    f32 playback_rate { 1.0f };
    f32 detune_cents { 0.0f };

    // Sample frames in the AudioBuffer's timeline.
    Optional<size_t> duration_in_sample_frames;

    // Sample frames in the AudioBuffer's timeline.
    size_t offset_frame { 0 };
    bool loop { false };
    size_t loop_start_frame { 0 };
    size_t loop_end_frame { 0 };

    // Buffer metadata.
    f32 sample_rate { 44100.0f };
    size_t channel_count { 1 };
    size_t length_in_sample_frames { 0 };

    // Handle to external buffer PCM.
    u64 buffer_id { 0 };

    ErrorOr<void> encode_wire_payload(WireEncoder&) const;
    static ErrorOr<AudioBufferSourceGraphNode> decode_wire_payload(WireDecoder&);

    OwnPtr<RenderNode> make_render_node(NodeID, size_t quantum_size, GraphResourceResolver const&) const;
    GraphUpdateKind classify_update(AudioBufferSourceGraphNode const& new_desc) const;

    template<typename SetState>
    void initialize_param_state(SetState&& set_state) const
    {
        set_state(AudioBufferSourceParamIndex::playback_rate, playback_rate, 0.0f, AK::NumericLimits<f32>::max());
        set_state(AudioBufferSourceParamIndex::detune, detune_cents, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
    }

    template<typename UpdateIntrinsic>
    void update_intrinsic_values(UpdateIntrinsic&& update_intrinsic) const
    {
        update_intrinsic(AudioBufferSourceParamIndex::playback_rate, playback_rate);
        update_intrinsic(AudioBufferSourceParamIndex::detune, detune_cents);
    }
};

}
