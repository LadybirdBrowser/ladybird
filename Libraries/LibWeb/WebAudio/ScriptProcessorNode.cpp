/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BaseAudioContext.h"

#include <AK/GenericShorthands.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ScriptProcessorNode);

ScriptProcessorNode::ScriptProcessorNode(GC::Ref<BaseAudioContext> context,
    u8 number_of_input_channels, u8 number_of_output_channels)
    : AudioNode(context)
    , m_number_of_input_channels(number_of_input_channels)
    , m_number_of_output_channels(number_of_output_channels)
{
}

ScriptProcessorNode::~ScriptProcessorNode() = default;

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

    return script_processor_node;
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
