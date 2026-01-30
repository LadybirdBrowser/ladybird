/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorHost.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorRequestPump.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorTransport.h>

namespace Web::WebAudio::Render {

void ScriptProcessorRequestPump::process(NodeID node_id, ScriptProcessorNodeState& state)
{
    ASSERT_CONTROL_THREAD();
    process_requests(node_id, state);
}

void ScriptProcessorRequestPump::process_requests(NodeID node_id, ScriptProcessorNodeState& state)
{
    ASSERT_CONTROL_THREAD();

    if (!m_host)
        return;

    if (!state.request_stream.is_valid() || !state.response_stream.is_valid())
        return;

    RenderContext render_process_context {};

    while (true) {
        auto desc = state.request_stream.try_receive_ready_block();
        if (!desc.has_value())
            break;

        auto block = state.request_stream.block_bytes(desc->block_index);
        if (block.is_empty() || desc->used_size < script_processor_request_fixed_bytes) {
            (void)state.request_stream.try_release_block_index(desc->block_index);
            continue;
        }

        auto const* header = reinterpret_cast<ScriptProcessorRequestHeader const*>(block.data());
        if (header->magic != script_processor_request_magic || header->version != script_processor_stream_version) {
            if (WebAudio::should_log_script_processor_bridge())
                dbgln("WebAudio: ScriptProcessor bad header node={} magic=0x{:x} version={}", node_id.value(), header->magic, header->version);
            (void)state.request_stream.try_release_block_index(desc->block_index);
            continue;
        }

        if (header->node_id != node_id.value() || header->buffer_size != state.buffer_size || header->input_channel_count != state.input_channel_count || header->output_channel_count != state.output_channel_count) {
            if (WebAudio::should_log_script_processor_bridge()) {
                dbgln("WebAudio: ScriptProcessor mismatch node={} got node={} bs={} in={} out={} expected bs={} in={} out={}",
                    node_id.value(),
                    header->node_id,
                    header->buffer_size,
                    header->input_channel_count,
                    header->output_channel_count,
                    state.buffer_size,
                    state.input_channel_count,
                    state.output_channel_count);
            }
            (void)state.request_stream.try_release_block_index(desc->block_index);
            continue;
        }

        size_t input_floats = static_cast<size_t>(state.buffer_size) * static_cast<size_t>(state.input_channel_count);
        size_t required_bytes = script_processor_request_fixed_bytes + (input_floats * sizeof(f32));
        if (desc->used_size < required_bytes || block.size() < required_bytes) {
            (void)state.request_stream.try_release_block_index(desc->block_index);
            continue;
        }

        auto const* input_f32 = reinterpret_cast<f32 const*>(block.data() + script_processor_request_fixed_bytes);

        Vector<ReadonlySpan<f32>> input_channels;
        input_channels.resize(state.input_channel_count);
        for (u32 ch = 0; ch < state.input_channel_count; ++ch)
            input_channels[ch] = ReadonlySpan<f32> { input_f32 + (static_cast<size_t>(ch) * state.buffer_size), state.buffer_size };

        Vector<Span<f32>> output_channels;
        output_channels.resize(state.output_channel_count);
        for (u32 ch = 0; ch < state.output_channel_count; ++ch)
            output_channels[ch] = Span<f32> { state.output_scratch.data() + (static_cast<size_t>(ch) * state.buffer_size), state.buffer_size };

        (void)m_host->process_script_processor(
            node_id,
            render_process_context,
            header->playback_time_seconds,
            state.buffer_size,
            state.input_channel_count,
            state.output_channel_count,
            input_channels.span(),
            output_channels.span());

        auto response_block_index = state.response_stream.try_acquire_block_index();
        if (!response_block_index.has_value()) {
            if (WebAudio::should_log_script_processor_bridge())
                dbgln("WebAudio: ScriptProcessor no free response block node={}", node_id.value());
            (void)state.request_stream.try_release_block_index(desc->block_index);
            continue;
        }

        auto response_block = state.response_stream.block_bytes(*response_block_index);
        size_t output_floats = static_cast<size_t>(state.buffer_size) * static_cast<size_t>(state.output_channel_count);
        size_t response_bytes = script_processor_response_fixed_bytes + (output_floats * sizeof(f32));
        if (response_block.size() < response_bytes) {
            if (WebAudio::should_log_script_processor_bridge())
                dbgln("WebAudio: ScriptProcessor response block too small node={} have={} need={}", node_id.value(), response_block.size(), response_bytes);
            (void)state.response_stream.try_release_block_index(*response_block_index);
            (void)state.request_stream.try_release_block_index(desc->block_index);
            continue;
        }

        auto* response_header = reinterpret_cast<ScriptProcessorResponseHeader*>(response_block.data());
        *response_header = ScriptProcessorResponseHeader {
            .magic = script_processor_response_magic,
            .version = script_processor_stream_version,
            .node_id = node_id.value(),
            .buffer_size = state.buffer_size,
            .output_channel_count = state.output_channel_count,
            .reserved0 = 0,
            .reserved1 = 0,
        };

        auto* output_f32 = reinterpret_cast<f32*>(response_block.data() + script_processor_response_fixed_bytes);
        __builtin_memcpy(output_f32, state.output_scratch.data(), output_floats * sizeof(f32));

        (void)state.response_stream.try_submit_ready_block(*response_block_index, static_cast<u32>(response_bytes));

        (void)state.request_stream.try_release_block_index(desc->block_index);
    }
}

} // namespace Web::WebAudio::Render
