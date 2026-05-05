/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/EventNames.h>
// #include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioProcessingEvent.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
// #include <LibWebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Script/OfflineScriptProcessorHost.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWebAudio/Debug.h>

namespace Web::WebAudio::Render {

bool OfflineScriptProcessorHost::process_script_processor(NodeID node_id, RenderContext&,
    double playback_time_seconds, size_t buffer_size,
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

    GC::Ref<AudioBuffer> input_buffer = MUST(AudioBuffer::create(*m_realm, input_channel_count, buffer_size, m_context->sample_rate()));
    GC::Ref<AudioBuffer> output_buffer = MUST(AudioBuffer::create(*m_realm, output_channel_count, buffer_size, m_context->sample_rate()));

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
    GC::Ref<AudioProcessingEvent> event = MUST(AudioProcessingEvent::construct_impl(*m_realm, HTML::EventNames::audioprocess, event_init));
    (void)node.dispatch_event(event);

    for (size_t ch = 0; ch < output_channel_count; ++ch) {
        GC::Ref<JS::Float32Array> channel_data = MUST(output_buffer->get_channel_data(ch));
        for (size_t i = 0; i < buffer_size; ++i)
            output_channels[ch][i] = channel_data->data()[i];
    }

    return true;
}

}
