/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/FrequencyAnalysis.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class AnalyserRenderNode final : public RenderNode {
public:
    AnalyserRenderNode(NodeID node_id, AnalyserGraphNode const& desc, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    void apply_description(GraphNodeDescription const& node) override;
    AudioBus const& output(size_t) const override { return m_output; }

    bool copy_analyser_time_domain_data(Span<f32> output) const;
    bool copy_analyser_frequency_data_db(Span<f32> output) const;

private:
    static constexpr size_t max_channel_capacity = 32;

    void update_time_domain_snapshot_buffer();
    void initialize_storage();
    void reset_runtime_state();
    static size_t bin_count_for_fft_size(size_t fft_size) { return fft_size / 2; }

    Atomic<size_t> m_fft_size { 2048 };
    Atomic<f32> m_smoothing_time_constant { 0.8f };
    AudioBus m_output;
    AudioBus m_analysis_mono;

    Vector<f32> m_ring_buffer;
    size_t m_ring_write_index { 0 };
    size_t m_ring_filled_samples { 0 };

    Atomic<u8> m_active_snapshot_index { 0 };

    // Render-thread smoothing state reset (e.g. when fft size changes).
    bool m_render_frequency_smoothing_needs_reset { true };

    // Render-thread snapshots (double-buffered). These are written on the render thread and read on the control thread.
    Array<Vector<f32>, 2> m_time_domain_cache;

    // Render-thread frequency analysis outputs (double-buffered).
    Vector<f32> m_previous_block_render;
    Array<Vector<f32>, 2> m_frequency_data_db;
    FrequencyAnalysisScratch m_frequency_scratch_render;
};

}
