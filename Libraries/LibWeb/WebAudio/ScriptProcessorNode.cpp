/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BaseAudioContext.h"

#include <AK/GenericShorthands.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibGC/Weak.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioProcessingEvent.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>
#include <LibWeb/WebAudio/Rendering/AudioWorkletPipe.h>
#include <LibWeb/WebAudio/Rendering/RealtimeAudioRenderer.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorPipe.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorRenderNode.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ScriptProcessorNode);

ScriptProcessorNode::ScriptProcessorNode(GC::Ref<BaseAudioContext> context,
    u8 number_of_input_channels, u8 number_of_output_channels)
    : AudioNode(context, number_of_input_channels)
    , m_number_of_input_channels(number_of_input_channels)
    , m_number_of_output_channels(number_of_output_channels)
{
}

ScriptProcessorNode::~ScriptProcessorNode() = default;

// Other engines have a latency of two buffers. A realtime context needs more when the device pulls a burst of audio
// that is longer than a buffer, since script cannot run in the middle of a burst.
static size_t block_count_for_context(BaseAudioContext const& context, size_t buffer_size)
{
    if (is<OfflineAudioContext>(context))
        return 2;
    auto quantum_size = BaseAudioContext::render_quantum_size();
    auto lookahead_frames = Rendering::AudioWorkletPipe::Config::ring_sizing_for_device_latency(
                                Rendering::RealtimeAudioRenderer::TARGET_LATENCY_MS, context.sample_rate(), quantum_size)
                                .prime_level
        * quantum_size;
    return max(ceil_div(lookahead_frames, buffer_size) + 1, 2uz);
}

WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> ScriptProcessorNode::create(
    GC::Ref<BaseAudioContext> context,
    WebIDL::Long buffer_size,
    WebIDL::UnsignedLong number_of_input_channels,
    WebIDL::UnsignedLong number_of_output_channels)
{
    auto script_processor_node = GC::Heap::the().allocate<ScriptProcessorNode>(context,
        static_cast<u8>(number_of_input_channels), static_cast<u8>(number_of_output_channels));

    script_processor_node->set_buffer_size_without_validation(buffer_size);

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
    // The channel count mode cannot be changed from "explicit" and an NotSupportedError exception MUST be thrown for
    // any attempt to change the value.
    TRY(script_processor_node->set_channel_count_mode(ChannelCountMode::Explicit));

    Rendering::ScriptProcessorPipe::Config pipe_config {
        .buffer_size = static_cast<size_t>(buffer_size),
        .input_channel_count = number_of_input_channels,
        .output_channel_count = number_of_output_channels,
        .block_count = block_count_for_context(context, buffer_size),
        .waits_for_processing = is<OfflineAudioContext>(*context),
    };
    auto pipe = Rendering::ScriptProcessorPipe::create(pipe_config, Core::EventLoop::current());
    pipe->set_process_callback([node = GC::Weak { *script_processor_node }](size_t block, u64 start_frame) {
        if (node)
            node->process_block(block, start_frame);
    });
    script_processor_node->m_pipe = pipe;
    context->add_script_processor_node(*script_processor_node);
    script_processor_node->queue_render_node_creation(make<Rendering::ScriptProcessorRenderNode>(
        script_processor_node->node_id(), BaseAudioContext::render_quantum_size(), move(pipe)));

    return script_processor_node;
}

void ScriptProcessorNode::finalize()
{
    if (m_pipe) {
        m_pipe->clear_process_callback();
        m_pipe->shut_down();
    }
    Base::finalize();
}

void ScriptProcessorNode::process_block(size_t block, u64 start_frame)
{
    auto& pipe = *m_pipe;
    auto const& config = pipe.config();

    if (!context()->relevant_window().associated_document().is_fully_active()) {
        pipe.hand_block_back_to_rendering_thread(block, false);
        return;
    }

    auto as_samples = [](Bytes bytes) {
        return Span<float> { reinterpret_cast<float*>(bytes.data()), bytes.size() / sizeof(float) };
    };

    auto sample_rate = context()->sample_rate();
    auto input_buffer = MUST(AudioBuffer::create(config.input_channel_count, config.buffer_size, sample_rate));
    for (size_t channel = 0; channel < config.input_channel_count; ++channel)
        pipe.input_channel(block, channel).copy_to(as_samples(MUST(input_buffer->channel_data(channel))));
    auto output_buffer = MUST(AudioBuffer::create(config.output_channel_count, config.buffer_size, sample_rate));

    auto playback_time = (start_frame + config.block_count * config.buffer_size) / static_cast<double>(sample_rate);

    {
        HTML::TemporaryExecutionContext execution_context { HTML::relevant_realm(relevant_global_object()), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        AudioProcessingEventInit event_init { {}, input_buffer, output_buffer, playback_time };
        auto event = AudioProcessingEvent::create(HTML::EventNames::audioprocess, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object()));
        dispatch_event(event);
    }

    // https://webaudio.github.io/web-audio-api/#acquire-the-content
    // When the dispatch of an AudioProcessingEvent completes, it acquires the contents of its outputBuffer.
    auto output = output_buffer->acquire_contents();
    if (!output) {
        pipe.hand_block_back_to_rendering_thread(block, false);
        return;
    }
    for (size_t channel = 0; channel < config.output_channel_count; ++channel)
        output->channels[channel].span().copy_to(pipe.output_channel(block, channel));
    pipe.hand_block_back_to_rendering_thread(block, true);
}

WebIDL::ExceptionOr<void> ScriptProcessorNode::validate_options(
    WebIDL::Long buffer_size,
    WebIDL::UnsignedLong number_of_input_channels,
    WebIDL::UnsignedLong number_of_output_channels)
{
    // https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
    // It is invalid for both numberOfInputChannels and numberOfOutputChannels to be zero. In this case an
    // IndexSizeError MUST be thrown.
    if (number_of_input_channels == 0 && number_of_output_channels == 0) {
        return WebIDL::IndexSizeError::create(
            "Number of input and output channels cannot both be zero in a ScriptProcessorNode"_utf16);
    }

    // This parameter determines the number of channels for this node’s input. The default value is 2. Values of up to
    // 32 must be supported. A NotSupportedError must be thrown if the number of channels is not supported.
    if (number_of_input_channels > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create("Invalid number of input channels"_utf16);

    // This parameter determines the number of channels for this node’s output. The default value is 2. Values of up to
    // 32 must be supported. A NotSupportedError must be thrown if the number of channels is not supported.
    if (number_of_output_channels > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create("Invalid number of output channels"_utf16);

    // The size of the buffer (in sample-frames) which needs to be processed each time audioprocess is fired. Legal
    // values are (256, 512, 1024, 2048, 4096, 8192, 16384).
    //
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createscriptprocessor
    // If the value of this parameter is not one of the allowed power-of-2 values listed above, an IndexSizeError MUST
    // be thrown.
    if (!first_is_one_of(buffer_size, 256, 512, 1024, 2048, 4096, 8192, 16384))
        return WebIDL::IndexSizeError::create("Unsupported buffer size for a ScriptProcessorNode"_utf16);

    return {};
}

// https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
WebIDL::UnsignedLong ScriptProcessorNode::channel_count() const
{
    // This is the number of channels specified when constructing this node.
    return m_number_of_input_channels;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> ScriptProcessorNode::set_channel_count(WebIDL::UnsignedLong)
{
    // ScriptProcessorNode: The channel count cannot be changed, and an NotSupportedError exception MUST be thrown for
    // any attempt to change the value.
    return WebIDL::InvalidStateError::create("Cannot modify channel count in a ScriptProcessorNode"_utf16);
}

WebIDL::ExceptionOr<void> ScriptProcessorNode::set_channel_count_mode(ChannelCountMode channel_count_mode)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
    // ScriptProcessorNode: The channel count mode cannot be changed from "explicit" and an NotSupportedError exception
    // MUST be thrown for any attempt to change the value.
    if (channel_count_mode != ChannelCountMode::Explicit)
        return WebIDL::InvalidStateError::create("Channel count mode must be 'explicit'"_utf16);

    return AudioNode::set_channel_count_mode(channel_count_mode);
}

// https://webaudio.github.io/web-audio-api/#dom-scriptprocessornode-onaudioprocess
GC::Ptr<WebIDL::CallbackType> ScriptProcessorNode::onaudioprocess()
{
    return event_handler_attribute(HTML::EventNames::audioprocess);
}

// https://webaudio.github.io/web-audio-api/#dom-scriptprocessornode-onaudioprocess
void ScriptProcessorNode::set_onaudioprocess(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::audioprocess, value);
}

// https://webaudio.github.io/web-audio-api/#dom-scriptprocessornode-buffersize
WebIDL::ExceptionOr<void> ScriptProcessorNode::set_buffer_size(WebIDL::Long buffer_size)
{
    // The size of the buffer (in sample-frames) which needs to be processed each time audioprocess is fired. Legal
    // values are (256, 512, 1024, 2048, 4096, 8192, 16384).

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createscriptprocessor
    // If the value of this parameter is not one of the allowed power-of-2 values listed above, an IndexSizeError MUST
    // be thrown.
    if (!first_is_one_of(buffer_size, 256, 512, 1024, 2048, 4096, 8192, 16384))
        return WebIDL::IndexSizeError::create("Unsupported buffer size for a ScriptProcessorNode"_utf16);

    m_buffer_size = buffer_size;
    return {};
}

}
