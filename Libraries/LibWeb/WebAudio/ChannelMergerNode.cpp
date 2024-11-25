/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ChannelMergerNode);

ChannelMergerNode::ChannelMergerNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ChannelMergerOptions const& options)
    : AudioNode(realm, context)
    , m_number_of_inputs(options.number_of_inputs)
{
}

ChannelMergerNode::~ChannelMergerNode() = default;

WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> ChannelMergerNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ChannelMergerOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> ChannelMergerNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ChannelMergerOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
    // An IndexSizeError exception MUST be thrown if numberOfInputs is less than 1 or is greater
    // than the number of supported channels.
    if (options.number_of_inputs < 1 || options.number_of_inputs > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::IndexSizeError::create(realm, "Invalid number of inputs"_string);

    auto node = realm.create<ChannelMergerNode>(realm, context, options);

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

// https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
WebIDL::ExceptionOr<void> ChannelMergerNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // The channel count cannot be changed, and an InvalidStateError exception MUST be thrown for
    // any attempt to change the value.
    if (channel_count != 1)
        return WebIDL::InvalidStateError::create(realm(), "Channel count cannot be changed"_string);

    return Base::set_channel_count(channel_count);
}

WebIDL::ExceptionOr<void> ChannelMergerNode::set_channel_count_mode(Bindings::ChannelCountMode channel_count_mode)
{
    // The channel count mode cannot be changed from "explicit" and an InvalidStateError exception
    // MUST be thrown for any attempt to change the value.
    if (channel_count_mode != Bindings::ChannelCountMode::Explicit)
        return WebIDL::InvalidStateError::create(realm(), "Channel count mode cannot be changed"_string);

    return Base::set_channel_count_mode(channel_count_mode);
}

}
