/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/SharedAudioBuffer.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class AudioBufferSourceRenderNode final : public RenderNode {
public:
    AudioBufferSourceRenderNode(NodeID node_id, AudioBufferSourceGraphNode const& desc, RefPtr<SharedAudioBuffer> buffer, size_t quantum_size);

    void process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const& param_inputs) override;
    AudioBus const& output(size_t) const override { return m_output; }
    void apply_description(GraphNodeDescription const& node) override;
    void schedule_start(Optional<size_t> start_frame) override
    {
        m_start_frame = start_frame.value_or(0);
        m_has_start = start_frame.has_value();
        m_start_time_in_context_frames = {};
    }
    void schedule_stop(Optional<size_t> stop_frame) override { m_stop_frame = stop_frame; }

private:
    bool m_has_start { false };

    // Base values for AudioParams (k-rate).
    f32 m_playback_rate { 1.0f };
    f32 m_detune_cents { 0.0f };

    size_t m_start_frame { 0 };
    Optional<size_t> m_stop_frame;
    size_t m_offset_frame { 0 };
    Optional<size_t> m_duration_in_sample_frames;

    bool m_loop { false };
    size_t m_loop_start_frame { 0 };
    size_t m_loop_end_frame { 0 };

    f32 m_sample_rate { 44100.0f };
    size_t m_channel_count { 0 };
    size_t m_length_in_sample_frames { 0 };
    RefPtr<SharedAudioBuffer> m_buffer;

    // Runtime playback state (render-thread owned).
    bool m_is_playing { false };
    bool m_finished { false };
    f64 m_playhead_in_sample_frames { 0.0 }; // Absolute position in buffer sample frames.
    f64 m_progress_in_sample_frames { 0.0 }; // Monotonic progress for duration tracking.

    AudioBus m_output;
    AudioBus m_playback_rate_input;
    AudioBus m_detune_input;

    SincResamplerKernel m_resampler_table;
    Optional<f64> m_last_resampler_increment;
    Optional<f64> m_start_time_in_context_frames;
};

}
