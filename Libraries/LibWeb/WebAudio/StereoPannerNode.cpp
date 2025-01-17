/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/StereoPannerNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(StereoPannerNode);

StereoPannerNode::~StereoPannerNode() = default;

WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> StereoPannerNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, StereoPannerOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-stereopannernode-stereopannernode
WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> StereoPannerNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, StereoPannerOptions const& options)
{
    // Create the node and allocate memory
    auto node = realm.create<StereoPannerNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#stereopannernode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::ClampedMax;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));
    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
WebIDL::ExceptionOr<void> StereoPannerNode::set_channel_count_mode(Bindings::ChannelCountMode mode)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
    // The channel count mode cannot be set to "max", and a NotSupportedError exception MUST be thrown for any attempt to set it to "max".
    if (mode == Bindings::ChannelCountMode::Max) {
        return WebIDL::NotSupportedError::create(realm(), "StereoPannerNode does not support 'max' as channelCountMode."_string);
    }

    // If the mode is valid, call the base class implementation
    return AudioNode::set_channel_count_mode(mode);
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> StereoPannerNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
    // The channel count cannot be greater than two, and a NotSupportedError exception MUST be thrown for any attempt to change it to a value greater than two.
    if (channel_count > 2) {
        return WebIDL::NotSupportedError::create(realm(), "StereoPannerNode does not support channel count greater than 2"_string);
    }

    return AudioNode::set_channel_count(channel_count);
}

StereoPannerNode::StereoPannerNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, StereoPannerOptions const& options)
    : AudioNode(realm, context)
    , m_pan(AudioParam::create(realm, context, options.pan, -1, 1, Bindings::AutomationRate::ARate))
{
}

void StereoPannerNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(StereoPannerNode);
}

void StereoPannerNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_pan);
}

}
