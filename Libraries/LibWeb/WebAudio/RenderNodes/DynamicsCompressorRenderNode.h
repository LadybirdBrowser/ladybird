/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class DynamicsCompressorRenderNode final : public RenderNode {
public:
    DynamicsCompressorRenderNode(NodeID node_id, DynamicsCompressorGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

    f32 reduction_db() const;

private:
    static constexpr size_t max_channel_count { 32 };
    static constexpr f32 lookahead_seconds { 0.006f };

    void ensure_delay_capacity(RenderContext const& context, size_t channel_count);

    static f32 linear_to_db(f32 linear_value);
    static f32 db_to_linear(f32 db_value);
    static f32 apply_compression_curve(f32 linear_input, f32 threshold_db, f32 knee_db, f32 ratio);
    static f32 compute_makeup_gain(f32 threshold_db, f32 knee_db, f32 ratio);

    f32 m_threshold_db { -24.0f };
    f32 m_knee_db { 30.0f };
    f32 m_ratio { 12.0f };
    f32 m_attack_seconds { 0.003f };
    f32 m_release_seconds { 0.25f };

    size_t m_channel_count { 1 };

    // Delay line storage for the fixed look-ahead.
    size_t m_ring_size { 0 };
    size_t m_write_index { 0 };
    size_t m_frames_written { 0 };
    Vector<Vector<f32>> m_ring;

    // Envelope follower state.
    f32 m_detector_average { 0.0f };
    f32 m_compressor_gain { 1.0f };

    Atomic<f32> m_reduction_db { 0.0f };

    AudioBus m_output;
    AudioBus m_threshold_input;
    AudioBus m_knee_input;
    AudioBus m_ratio_input;
    AudioBus m_attack_input;
    AudioBus m_release_input;
};

}
