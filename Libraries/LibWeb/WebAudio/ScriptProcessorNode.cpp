/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BaseAudioContext.h"

#include <AK/GenericShorthands.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ScriptProcessorNodePrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ScriptProcessorNode);

ScriptProcessorNode::ScriptProcessorNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context,
    u8 number_of_input_channels, u8 number_of_output_channels)
    : AudioNode(realm, context)
    , m_number_of_input_channels(number_of_input_channels)
    , m_number_of_output_channels(number_of_output_channels)
{
}

ScriptProcessorNode::~ScriptProcessorNode() = default;

WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> ScriptProcessorNode::create(JS::Realm& realm,
    GC::Ref<BaseAudioContext> context, WebIDL::Long buffer_size, WebIDL::UnsignedLong number_of_input_channels,
    WebIDL::UnsignedLong number_of_output_channels)
{
    // https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
    // It is invalid for both numberOfInputChannels and numberOfOutputChannels to be zero. In this case an
    // IndexSizeError MUST be thrown.
    if (number_of_input_channels == 0 && number_of_output_channels == 0) {
        return WebIDL::IndexSizeError::create(realm,
            "Number of input and output channels cannot both be zero in a ScriptProcessorNode"_string);
    }

    // This parameter determines the number of channels for this node’s input. The default value is 2. Values of up to
    // 32 must be supported. A NotSupportedError must be thrown if the number of channels is not supported.
    if (number_of_input_channels > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm, "Invalid number of input channels"_string);

    // This parameter determines the number of channels for this node’s output. The default value is 2. Values of up to
    // 32 must be supported. A NotSupportedError must be thrown if the number of channels is not supported.
    if (number_of_output_channels > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm, "Invalid number of output channels"_string);

    auto script_processor_node = realm.create<ScriptProcessorNode>(realm, context,
        number_of_input_channels, number_of_output_channels);

    TRY(script_processor_node->set_buffer_size(buffer_size));

    // https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
    // The channel count mode cannot be changed from "explicit" and an NotSupportedError exception MUST be thrown for
    // any attempt to change the value.
    TRY(script_processor_node->set_channel_count_mode(Bindings::ChannelCountMode::Explicit));

    return script_processor_node;
}

void ScriptProcessorNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ScriptProcessorNode);
    Base::initialize(realm);
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
    return WebIDL::InvalidStateError::create(realm(),
        "Cannot modify channel count in a ScriptProcessorNode"_string);
}

WebIDL::ExceptionOr<void> ScriptProcessorNode::set_channel_count_mode(Bindings::ChannelCountMode channel_count_mode)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
    // ScriptProcessorNode: The channel count mode cannot be changed from "explicit" and an NotSupportedError exception
    // MUST be thrown for any attempt to change the value.
    if (channel_count_mode != Bindings::ChannelCountMode::Explicit)
        return WebIDL::InvalidStateError::create(realm(), "Channel count mode must be 'explicit'"_string);

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
        return WebIDL::IndexSizeError::create(realm(), "Unsupported buffer size for a ScriptProcessorNode"_string);

    m_buffer_size = buffer_size;
    return {};
}

}
