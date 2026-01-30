/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/StreamTransportNotify.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorTransport.h>
#include <WebAudioWorker/SessionScriptProcessorHost.h>
#include <WebAudioWorker/WebAudioSession.h>

namespace WebAudioWorker {

SessionScriptProcessorHost::SessionScriptProcessorHost(WebAudioSession& session)
    : m_session(session)
{
}

bool SessionScriptProcessorHost::process_script_processor(
    Web::WebAudio::NodeID node_id,
    Web::WebAudio::Render::RenderContext& render_process_context,
    double playback_time_seconds,
    size_t buffer_size,
    size_t input_channel_count,
    size_t output_channel_count,
    Span<ReadonlySpan<f32>> input_channels,
    Span<Span<f32>> output_channels)
{
    ++m_session.m_script_processor_processed_blocks;
    (void)render_process_context;

    if (Web::WebAudio::should_log_script_processor_bridge()) {
        dbgln("client_cid={}: ScriptProcessor request session={} node={} t={} bs={} in={} out={}",
            m_session.m_client_id,
            m_session.m_session_id,
            node_id.value(),
            playback_time_seconds,
            buffer_size,
            input_channel_count,
            output_channel_count);
    }

    Core::SharedBufferStream request_stream;
    Core::SharedBufferStream response_stream;
    int notify_write_fd = -1;
    u32 expected_buffer_size = 0;
    u32 expected_input_channel_count = 0;
    u32 expected_output_channel_count = 0;

    StreamState::ScriptProcessorStreamMap* index = m_session.m_streams.script_processor_streams.load(AK::MemoryOrder::memory_order_acquire);
    if (!index) {
        if (Web::WebAudio::should_log_script_processor_bridge())
            dbgln("client_cid={}: ScriptProcessor missing stream index session={} node={}", m_session.m_client_id, m_session.m_session_id, node_id.value());
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    index->ref();
    auto it = index->streams.find(node_id.value());
    if (it == index->streams.end()) {
        index->unref();
        if (Web::WebAudio::should_log_script_processor_bridge())
            dbgln("client_cid={}: ScriptProcessor missing stream state session={} node={}", m_session.m_client_id, m_session.m_session_id, node_id.value());
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    auto const& state = it->value;
    request_stream = state.request_stream;
    response_stream = state.response_stream;
    notify_write_fd = state.descriptor.request_notify_write_fd.fd();
    expected_buffer_size = state.descriptor.buffer_size;
    expected_input_channel_count = state.descriptor.input_channel_count;
    expected_output_channel_count = state.descriptor.output_channel_count;
    index->unref();

    if (!request_stream.is_valid() || !response_stream.is_valid() || notify_write_fd < 0) {
        if (Web::WebAudio::should_log_script_processor_bridge())
            dbgln("cid={}: ScriptProcessor missing streams session={} node={} notify_fd={}", m_session.m_client_id, m_session.m_session_id, node_id.value(), notify_write_fd);
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    if (buffer_size == 0 || output_channel_count == 0) {
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    if (buffer_size != expected_buffer_size || input_channel_count != expected_input_channel_count || output_channel_count != expected_output_channel_count) {
        if (Web::WebAudio::should_log_script_processor_bridge()) {
            dbgln("cid={}: ScriptProcessor mismatch session={} node={} got bs={} in={} out={} expected bs={} in={} out={}",
                m_session.m_client_id,
                m_session.m_session_id,
                node_id.value(),
                buffer_size,
                input_channel_count,
                output_channel_count,
                expected_buffer_size,
                expected_input_channel_count,
                expected_output_channel_count);
        }
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    auto request_block_index = request_stream.try_acquire_block_index();
    if (!request_block_index.has_value()) {
        if (Web::WebAudio::should_log_script_processor_bridge())
            dbgln("cid={}: ScriptProcessor no free request block session={} node={}", m_session.m_client_id, m_session.m_session_id, node_id.value());
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    auto request_block = request_stream.block_bytes(*request_block_index);
    size_t input_floats = buffer_size * input_channel_count;
    size_t request_bytes = Web::WebAudio::Render::script_processor_request_fixed_bytes + (input_floats * sizeof(f32));
    if (request_block.size() < request_bytes) {
        if (Web::WebAudio::should_log_script_processor_bridge())
            dbgln("cid={}: ScriptProcessor request block too small session={} node={} have={} need={}", m_session.m_client_id, m_session.m_session_id, node_id.value(), request_block.size(), request_bytes);
        (void)request_stream.try_release_block_index(*request_block_index);
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    auto* request_header = reinterpret_cast<Web::WebAudio::Render::ScriptProcessorRequestHeader*>(request_block.data());
    *request_header = Web::WebAudio::Render::ScriptProcessorRequestHeader {
        .magic = Web::WebAudio::Render::script_processor_request_magic,
        .version = Web::WebAudio::Render::script_processor_stream_version,
        .node_id = node_id.value(),
        .playback_time_seconds = playback_time_seconds,
        .buffer_size = static_cast<u32>(buffer_size),
        .input_channel_count = static_cast<u32>(input_channel_count),
        .output_channel_count = static_cast<u32>(output_channel_count),
        .reserved0 = 0,
    };

    auto* input_f32 = reinterpret_cast<f32*>(request_block.data() + Web::WebAudio::Render::script_processor_request_fixed_bytes);
    for (size_t ch = 0; ch < input_channel_count; ++ch) {
        auto slice = Span<f32> { input_f32 + (ch * buffer_size), buffer_size };
        input_channels[ch].slice(0, buffer_size).copy_to(slice);
    }

    if (!request_stream.try_submit_ready_block(*request_block_index, static_cast<u32>(request_bytes))) {
        if (Web::WebAudio::should_log_script_processor_bridge())
            dbgln("cid={}: ScriptProcessor failed to submit request session={} node={}", m_session.m_client_id, m_session.m_session_id, node_id.value());
        (void)request_stream.try_release_block_index(*request_block_index);
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    (void)Web::WebAudio::Render::try_signal_stream_notify_fd(notify_write_fd);

    bool have_response = false;
    u32 drained_responses = 0;
    u32 valid_responses = 0;
    u32 invalid_responses = 0;
    while (true) {
        auto response_desc = response_stream.try_receive_ready_block();
        if (!response_desc.has_value())
            break;

        ++drained_responses;

        auto response_block = response_stream.block_bytes(response_desc->block_index);
        size_t output_floats = buffer_size * output_channel_count;
        size_t response_bytes = Web::WebAudio::Render::script_processor_response_fixed_bytes + (output_floats * sizeof(f32));
        if (!response_block.is_empty() && response_desc->used_size >= response_bytes && response_block.size() >= response_bytes) {
            auto const* response_header = reinterpret_cast<Web::WebAudio::Render::ScriptProcessorResponseHeader const*>(response_block.data());
            if (response_header->magic == Web::WebAudio::Render::script_processor_response_magic
                && response_header->version == Web::WebAudio::Render::script_processor_stream_version
                && response_header->node_id == node_id.value()
                && response_header->buffer_size == buffer_size
                && response_header->output_channel_count == output_channel_count) {
                auto const* output_f32 = reinterpret_cast<f32 const*>(response_block.data() + Web::WebAudio::Render::script_processor_response_fixed_bytes);
                for (size_t ch = 0; ch < output_channel_count; ++ch) {
                    auto slice = ReadonlySpan<f32> { output_f32 + (ch * buffer_size), buffer_size };
                    slice.copy_to(output_channels[ch].slice(0, buffer_size));
                }
                have_response = true;
                ++valid_responses;
            } else {
                ++invalid_responses;
            }
        }

        (void)response_stream.try_release_block_index(response_desc->block_index);
    }

    if (!have_response) {
        for (auto span : output_channels)
            span.fill(0);
    }

    if (Web::WebAudio::should_log_script_processor_bridge()) {
        dbgln("cid={}: ScriptProcessor response {} session={} node={} drained={} valid={} invalid={}",
            m_session.m_client_id,
            have_response ? "ok" : "missing",
            m_session.m_session_id,
            node_id.value(),
            drained_responses,
            valid_responses,
            invalid_responses);
    }

    return have_response;
}

}
