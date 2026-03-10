/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/ScriptProcessorRenderNode.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorHost.h>

namespace Web::WebAudio::Render {

ScriptProcessorRenderNode::ScriptProcessorRenderNode(NodeID node_id, ScriptProcessorGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_quantum_size(quantum_size)
    , m_buffer_size(max<size_t>(256, desc.buffer_size))
    , m_input_channel_count(clamp<size_t>(desc.input_channel_count, 1, max_channel_count))
    , m_output_channel_count(clamp<size_t>(desc.output_channel_count, 1, max_channel_count))
    , m_quantum_input_mix(m_input_channel_count, quantum_size, max_channel_count)
    , m_quantum_output(m_output_channel_count, quantum_size, max_channel_count)
    , m_input_block(m_input_channel_count, m_buffer_size, max_channel_count)
{
    // ScriptProcessorNode legal sizes are powers of two in [256, 16384] and are all multiples of 128.
    // If we ever encounter a non-quantum-aligned buffer size, degrade to silence.
    if (m_buffer_size % quantum_size != 0)
        m_buffer_size = 0;
}

AudioBus const& ScriptProcessorRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();

    return m_quantum_output;
}

void ScriptProcessorRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    m_quantum_output.zero();

    if (m_buffer_size == 0)
        return;

    mix_input_for_quantum(inputs);
    append_quantum_to_input_block();

    // Emit output for this quantum from the current output block (or silence).
    write_quantum_output_from_current_block();

    // If we completed an input block this quantum, process it.
    if (m_input_block_offset_frames == m_buffer_size)
        process_completed_input_block(context);

    // Advance block cursors at bufferSize boundaries.
    advance_block_cursors_if_needed();
}

void ScriptProcessorRenderNode::mix_input_for_quantum(Vector<Vector<AudioBus const*>> const& inputs)
{
    ASSERT_RENDER_THREAD();

    m_quantum_input_mix.set_channel_count(m_input_channel_count);
    m_quantum_input_mix.zero();

    // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    if (!mixed_input)
        return;

    // Up-mix/down-mix from the edge-mixed bus into our fixed channel count.
    // This is important for cases like mono media sources feeding a 2-channel ScriptProcessor.
    AudioBus const* sources[] = { mixed_input };
    mix_inputs_into(m_quantum_input_mix, ReadonlySpan<AudioBus const*> { sources, 1 });
}

void ScriptProcessorRenderNode::append_quantum_to_input_block()
{
    ASSERT_RENDER_THREAD();

    size_t const remaining = m_buffer_size - m_input_block_offset_frames;
    size_t const to_copy = min(m_quantum_size, remaining);

    for (size_t ch = 0; ch < m_input_channel_count; ++ch) {
        auto src = m_quantum_input_mix.channel(ch).slice(0, to_copy);
        auto dst = m_input_block.channel(ch).slice(m_input_block_offset_frames, to_copy);
        src.copy_to(dst);
    }

    m_input_block_offset_frames += to_copy;

    // If quantum_size ever exceeded the remaining frames (shouldn't happen with legal sizes),
    // just drop the tail.
    if (to_copy < m_quantum_size)
        return;
}

void ScriptProcessorRenderNode::process_completed_input_block(RenderContext& context)
{
    ASSERT_RENDER_THREAD();

    if (context.script_processor_host) {
        auto output_block = make<AudioBus>(m_output_channel_count, m_buffer_size, max_channel_count);
        output_block->set_channel_count(m_output_channel_count);
        output_block->zero();

        Vector<ReadonlySpan<f32>, 32> input_channels;
        input_channels.resize(m_input_channel_count);
        for (size_t ch = 0; ch < m_input_channel_count; ++ch)
            input_channels[ch] = m_input_block.channel(ch);

        Vector<Span<f32>, 32> output_channels;
        output_channels.resize(m_output_channel_count);
        for (size_t ch = 0; ch < m_output_channel_count; ++ch)
            output_channels[ch] = output_block->channel(ch);

        f64 const latency_frames = static_cast<f64>(2 * m_buffer_size);
        f64 const playback_time_seconds = (static_cast<f64>(m_input_block_index * m_buffer_size) + latency_frames) / static_cast<f64>(context.sample_rate);

        bool ok = context.script_processor_host->process_script_processor(
            node_id(),
            context,
            playback_time_seconds,
            m_buffer_size,
            m_input_channel_count,
            m_output_channel_count,
            input_channels.span(),
            output_channels.span());

        if (!ok) {
            bool const input_all_zeros = is_all_zeros(m_input_block);
            WA_SP_DBGLN("[WebAudio][SP] process failed: node={} t={}s ctx_frame={} buffer={} in_ch={} out_ch={} input_all_zeros={} pending_outputs={} initial_silent_remaining={}",
                node_id(),
                playback_time_seconds,
                context.current_frame,
                m_buffer_size,
                m_input_channel_count,
                m_output_channel_count,
                input_all_zeros,
                m_pending_output_blocks.size(),
                m_initial_silent_blocks_remaining);
        }

        if (ok)
            m_pending_output_blocks.enqueue(move(output_block));
    }

    m_input_block.zero();
    m_input_block_offset_frames = 0;
    ++m_input_block_index;
}

void ScriptProcessorRenderNode::write_quantum_output_from_current_block()
{
    ASSERT_RENDER_THREAD();

    size_t const remaining = m_buffer_size - m_output_block_offset_frames;
    size_t const to_copy = min(m_quantum_size, remaining);

    if (m_current_output_block) {
        for (size_t ch = 0; ch < m_output_channel_count; ++ch) {
            auto src = m_current_output_block->channel(ch).slice(m_output_block_offset_frames, to_copy);
            auto dst = m_quantum_output.channel(ch).slice(0, to_copy);
            src.copy_to(dst);
        }
    }

    m_output_block_offset_frames += to_copy;
}

void ScriptProcessorRenderNode::advance_block_cursors_if_needed()
{
    ASSERT_RENDER_THREAD();

    if (m_output_block_offset_frames < m_buffer_size)
        return;

    // End of an output block.
    m_output_block_offset_frames = 0;
    ++m_output_block_index;

    if (m_initial_silent_blocks_remaining > 0) {
        --m_initial_silent_blocks_remaining;
        if (m_initial_silent_blocks_remaining > 0) {
            m_current_output_block = nullptr;
            return;
        }
        // We just consumed the last required silent block.
        // Fall through to dequeue the first processed output block (if available).
    }

    if (m_pending_output_blocks.is_empty()) {
        m_current_output_block = nullptr;
        return;
    }

    m_current_output_block = m_pending_output_blocks.dequeue();
}

}
