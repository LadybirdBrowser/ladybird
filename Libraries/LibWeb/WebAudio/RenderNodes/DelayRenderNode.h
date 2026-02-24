/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class DelayRenderNode final : public RenderNode {
public:
    DelayRenderNode(NodeID node_id, DelayGraphNode const& desc, size_t quantum_size);

    void process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    void process_cycle_writer(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs);
    void process_cycle_reader(RenderContext& context, Vector<Vector<AudioBus const*>> const& param_inputs, bool clamp_to_quantum);
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

private:
    void ensure_buffer_capacity(RenderContext const& context);

    f32 m_delay_time_seconds { 0.0f };
    f32 m_max_delay_time_seconds { 1.0f };

    size_t m_channel_count { 1 };
    size_t m_last_input_channels { 1 };

    // Delay line storage.
    size_t m_ring_size { 0 };
    size_t m_write_index { 0 };
    size_t m_frames_written { 0 };
    Vector<Vector<f32>> m_ring;

    AudioBus m_output;
    AudioBus m_delay_time_input;
};

}
