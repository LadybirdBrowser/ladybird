/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/SharedAudioBuffer.h>
#include <LibWeb/WebAudio/GraphNodes/ConvolverGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class ConvolverRenderNode final : public RenderNode {
public:
    ConvolverRenderNode(NodeID node_id, ConvolverGraphNode const& desc, RefPtr<SharedAudioBuffer> impulse_buffer, size_t quantum_size);

    void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;

private:
    void load_impulse_from_buffer(SharedAudioBuffer const* buffer);
    void renormalize_impulse();
    void ensure_impulse_channels(size_t channels);
    void rebuild_partitioned_impulse();

    struct FFTBlock {
        Vector<f64> real;
        Vector<f64> imag;
    };

    static constexpr size_t max_channels { 4 };

    bool m_normalize { true };
    ChannelInterpretation m_channel_interpretation { ChannelInterpretation::Speakers };
    size_t m_channel_count { 2 };
    RefPtr<SharedAudioBuffer> m_impulse_buffer;
    size_t m_impulse_buffer_channel_count { 0 };

    // Normalized impulse response, one channel per vector.
    Vector<Vector<f32>> m_impulse;
    size_t m_impulse_length { 0 };

    size_t m_partition_size { 0 };
    size_t m_fft_size { 0 };
    size_t m_partition_count { 0 };

    Vector<Vector<FFTBlock>> m_impulse_partitions;
    Vector<Vector<FFTBlock>> m_input_fft_history;
    size_t m_fft_history_write_index { 0 };

    Vector<Vector<f32>> m_overlap_tail;

    Vector<f64> m_fft_accum_real;
    Vector<f64> m_fft_accum_imag;
    Vector<f64> m_fft_time_real;
    Vector<f64> m_fft_time_imag;

    size_t m_output_channel_hold_frames { 0 };
    size_t m_tail_frames_remaining { 0 };

    size_t m_last_output_channels { 1 };

    AudioBus m_output;
};

}
