/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ChannelSplitterNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ChannelSplitterNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ChannelSplitterNode);

ChannelSplitterNode::ChannelSplitterNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ChannelSplitterOptions const& options)
    : AudioNode(realm, context)
    , m_number_of_outputs(options.number_of_outputs)
{
}

ChannelSplitterNode::~ChannelSplitterNode() = default;

WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> ChannelSplitterNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ChannelSplitterOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> ChannelSplitterNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ChannelSplitterOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
    // An IndexSizeError exception MUST be thrown if numberOfOutputs is less than 1 or is greater than the number of supported channels.
    if (options.number_of_outputs < 1 || options.number_of_outputs > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::IndexSizeError::create(realm, "Invalid number of outputs"_string);

    auto node = realm.create<ChannelSplitterNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#ChannelSplitterNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Explicit;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Discrete;
    default_options.channel_count = node->number_of_outputs();
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

void ChannelSplitterNode::initialize(JS::Realm& realm)
{
    AudioNode::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ChannelSplitterNode);
}

WebIDL::ExceptionOr<void> ChannelSplitterNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
    // The channel count cannot be changed, and an InvalidStateError exception MUST be thrown for any attempt to change the value.
    if (channel_count != m_number_of_outputs)
        return WebIDL::InvalidStateError::create(realm(), "Channel count must be equal to number of outputs"_string);

    return AudioNode::set_channel_count(channel_count);
}

WebIDL::ExceptionOr<void> ChannelSplitterNode::set_channel_count_mode(Bindings::ChannelCountMode channel_count_mode)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
    // The channel count mode cannot be changed from "explicit" and an InvalidStateError exception MUST be thrown for any attempt to change the value.
    if (channel_count_mode != Bindings::ChannelCountMode::Explicit)
        return WebIDL::InvalidStateError::create(realm(), "Channel count mode must be 'explicit'"_string);

    return AudioNode::set_channel_count_mode(channel_count_mode);
}

WebIDL::ExceptionOr<void> ChannelSplitterNode::set_channel_interpretation(Bindings::ChannelInterpretation channel_interpretation)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelinterpretation-constraints
    // The channel intepretation can not be changed from "discrete" and a InvalidStateError exception MUST be thrown for any attempt to change the value.
    if (channel_interpretation != Bindings::ChannelInterpretation::Discrete)
        return WebIDL::InvalidStateError::create(realm(), "Channel interpretation must be 'discrete'"_string);

    return AudioNode::set_channel_interpretation(channel_interpretation);
}

}
