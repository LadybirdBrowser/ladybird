/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Queue.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

class ScriptProcessorRenderNode final : public RenderNode {
public:
    ScriptProcessorRenderNode(NodeID, ScriptProcessorGraphNode const&, size_t quantum_size);

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) override;

    virtual size_t output_count() const override { return 1; }
    virtual AudioBus const& output(size_t output_index) const override;

private:
    static constexpr size_t max_channel_count { 32 };

    void mix_input_for_quantum(Vector<Vector<AudioBus const*>> const& inputs);
    void append_quantum_to_input_block();
    void process_completed_input_block(RenderContext&);

    void write_quantum_output_from_current_block();
    void advance_block_cursors_if_needed();

    size_t m_quantum_size { 0 };

    size_t m_buffer_size { 0 };
    size_t m_input_channel_count { 1 };
    size_t m_output_channel_count { 1 };

    // Per-quantum mixed input and produced output.
    AudioBus m_quantum_input_mix;
    AudioBus m_quantum_output;

    // Input block accumulation.
    AudioBus m_input_block;
    size_t m_input_block_offset_frames { 0 };
    u64 m_input_block_index { 0 };

    // Output block latency pipeline.
    Queue<OwnPtr<AudioBus>> m_pending_output_blocks;
    OwnPtr<AudioBus> m_current_output_block;
    size_t m_output_block_offset_frames { 0 };
    u64 m_output_block_index { 0 };
    u8 m_initial_silent_blocks_remaining { 2 };
};

}
