/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioProcessingEvent.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorHost.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>

namespace Web::WebAudio::Render {

bool OfflineScriptProcessorHost::process_script_processor(
    NodeID node_id,
    RenderContext&,
    double playback_time_seconds,
    size_t buffer_size,
    size_t input_channel_count,
    size_t output_channel_count,
    Span<ReadonlySpan<f32>> input_channels,
    Span<Span<f32>> output_channels)
{
    auto it = m_nodes.find(node_id);
    if (it == m_nodes.end()) {
        for (auto channel : output_channels)
            channel.fill(0);
        return false;
    }

    ScriptProcessorNode& node = *it->value;

    GC::Ref<AudioBuffer> input_buffer = MUST(AudioBuffer::create(m_realm, input_channel_count, buffer_size, m_context.sample_rate()));
    GC::Ref<AudioBuffer> output_buffer = MUST(AudioBuffer::create(m_realm, output_channel_count, buffer_size, m_context.sample_rate()));

    for (size_t ch = 0; ch < input_channel_count; ++ch) {
        GC::Ref<JS::Float32Array> channel_data = MUST(input_buffer->get_channel_data(ch));
        for (size_t i = 0; i < buffer_size; ++i)
            channel_data->data()[i] = input_channels[ch][i];
    }

    AudioProcessingEventInit event_init {
        {
            .bubbles = false,
            .cancelable = false,
            .composed = false,
        },
        playback_time_seconds,
        input_buffer,
        output_buffer,
    };
    GC::Ref<AudioProcessingEvent> event = MUST(AudioProcessingEvent::construct_impl(m_realm, HTML::EventNames::audioprocess, event_init));
    (void)node.dispatch_event(event);

    for (size_t ch = 0; ch < output_channel_count; ++ch) {
        GC::Ref<JS::Float32Array> channel_data = MUST(output_buffer->get_channel_data(ch));
        for (size_t i = 0; i < buffer_size; ++i)
            output_channels[ch][i] = channel_data->data()[i];
    }

    return true;
}

bool RealtimeScriptProcessorHost::process_script_processor(NodeID node_id, RenderContext&, double playback_time_seconds, size_t buffer_size, size_t input_channel_count, size_t output_channel_count, Span<ReadonlySpan<f32>> input_channels, Span<Span<f32>> output_channels)
{
    if (output_channel_count == 0 || buffer_size == 0) {
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    auto request = adopt_ref(*new Request);
    request->sequence = m_next_sequence.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) + 1;
    request->node_id = node_id;
    request->playback_time_seconds = playback_time_seconds;
    request->buffer_size = buffer_size;
    request->input_channel_count = input_channel_count;
    request->output_channel_count = output_channel_count;

    if (should_log_script_processor_bridge() && should_log_zero_detector() && input_channel_count > 0 && !input_channels.is_empty()) {
        if (is_all_zeros(input_channels[0])) {
            i64 now_ms = AK::MonotonicTime::now().milliseconds();
            i64 last_ms = m_last_zero_input_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
            if ((now_ms - last_ms) >= 250 && m_last_zero_input_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
                WA_SP_DBGLN("[WebAudio][SP][ZERO] input all zeros: seq={} node={} t={}s", request->sequence, node_id, playback_time_seconds);
        }
    }

    request->input_data.resize(input_channel_count);
    for (size_t ch = 0; ch < input_channel_count; ++ch) {
        request->input_data[ch].resize(buffer_size);
        input_channels[ch].slice(0, buffer_size).copy_to(request->input_data[ch]);
    }

    request->output_spans.resize(output_channel_count);
    for (size_t ch = 0; ch < output_channel_count; ++ch)
        request->output_spans[ch] = output_channels[ch].slice(0, buffer_size);

    if (current_thread_is_control_thread())
        return perform_request_on_control_thread(*request);

    auto strong_loop = m_control_event_loop->take();
    if (!strong_loop) {
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    strong_loop->deferred_invoke([this, request] {
        ASSERT_CONTROL_THREAD();

        bool ok = perform_request_on_control_thread(*request);

        Threading::MutexLocker locker { request->mutex };
        request->ok = ok;
        request->done = true;
        request->completed.signal();
    });

    Threading::MutexLocker locker { request->mutex };
    request->completed.wait_while_for([&] { return !request->done; }, AK::Duration::from_milliseconds(Render::REALTIME_SCRIPT_PROCESSOR_HOST_WAIT_TIMEOUT_MS));
    if (!request->done) {
        WA_SP_DBGLN("[WebAudio][SP] timeout waiting for control thread: seq={} node={} t={}s buffer={}", request->sequence, node_id, playback_time_seconds, buffer_size);
        for (auto span : output_channels)
            span.fill(0);
        return false;
    }

    return request->ok;
}

bool RealtimeScriptProcessorHost::perform_request_on_control_thread(Request& request)
{
    ASSERT_CONTROL_THREAD();

    bool ok = false;

    auto it = m_nodes.find(request.node_id);
    if (it != m_nodes.end() && it->value.ptr()) {
        ScriptProcessorNode& node = *it->value;

        HTML::TemporaryExecutionContext execution_context(m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        GC::Ref<AudioBuffer> input_buffer = MUST(AudioBuffer::create(m_realm, request.input_channel_count, request.buffer_size, m_context.sample_rate()));
        GC::Ref<AudioBuffer> output_buffer = MUST(AudioBuffer::create(m_realm, request.output_channel_count, request.buffer_size, m_context.sample_rate()));

        for (size_t ch = 0; ch < request.input_channel_count; ++ch) {
            GC::Ref<JS::Float32Array> channel_data = MUST(input_buffer->get_channel_data(ch));
            for (size_t i = 0; i < request.buffer_size; ++i)
                channel_data->data()[i] = request.input_data[ch][i];
        }

        AudioProcessingEventInit event_init {
            {
                .bubbles = false,
                .cancelable = false,
                .composed = false,
            },
            request.playback_time_seconds,
            input_buffer,
            output_buffer,
        };
        GC::Ref<AudioProcessingEvent> event = MUST(AudioProcessingEvent::construct_impl(m_realm, HTML::EventNames::audioprocess, event_init));
        (void)node.dispatch_event(event);

        for (size_t ch = 0; ch < request.output_channel_count; ++ch) {
            GC::Ref<JS::Float32Array> channel_data = MUST(output_buffer->get_channel_data(ch));
            for (size_t i = 0; i < request.buffer_size; ++i)
                request.output_spans[ch][i] = channel_data->data()[i];
        }

        ok = true;
    } else {
        for (auto span : request.output_spans)
            span.fill(0);
        ok = false;
    }

    return ok;
}

}
