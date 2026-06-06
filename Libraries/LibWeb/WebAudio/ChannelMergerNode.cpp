/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ChannelMergerNode);

ChannelMergerNode::ChannelMergerNode(GC::Ref<BaseAudioContext> context, Bindings::ChannelMergerOptions const& options)
    : AudioNode(context)
    , m_number_of_inputs(options.number_of_inputs)
{
}

ChannelMergerNode::~ChannelMergerNode() = default;

WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> ChannelMergerNode::create(GC::Ref<BaseAudioContext> context, Bindings::ChannelMergerOptions const& options)
{
    auto node = GC::Heap::the().allocate<ChannelMergerNode>(context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#BiquadFilterNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Explicit;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 1;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

WebIDL::ExceptionOr<void> ChannelMergerNode::validate_options(Bindings::ChannelMergerOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
    // An IndexSizeError exception MUST be thrown if numberOfInputs is less than 1 or is greater
    // than the number of supported channels.
    if (options.number_of_inputs < 1 || options.number_of_inputs > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::IndexSizeError::create("Invalid number of inputs"_utf16);

    return {};
}

WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> ChannelMergerNode::construct_impl(GC::Ref<BaseAudioContext> context, Bindings::ChannelMergerOptions const& options)
{
    TRY(validate_options(options));
    return create(context, options);
}

// https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
WebIDL::ExceptionOr<void> ChannelMergerNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // The channel count cannot be changed, and an InvalidStateError exception MUST be thrown for
    // any attempt to change the value.
    if (channel_count != 1)
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(relevant_global_object()), "Channel count cannot be changed"_utf16);

    return Base::set_channel_count(channel_count);
}

WebIDL::ExceptionOr<void> ChannelMergerNode::set_channel_count_mode(Bindings::ChannelCountMode channel_count_mode)
{
    // The channel count mode cannot be changed from "explicit" and an InvalidStateError exception
    // MUST be thrown for any attempt to change the value.
    if (channel_count_mode != Bindings::ChannelCountMode::Explicit)
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(relevant_global_object()), "Channel count mode cannot be changed"_utf16);

    return Base::set_channel_count_mode(channel_count_mode);
}

}
