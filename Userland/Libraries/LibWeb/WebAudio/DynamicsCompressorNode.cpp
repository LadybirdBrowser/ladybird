/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DynamicsCompressorNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(DynamicsCompressorNode);

DynamicsCompressorNode::~DynamicsCompressorNode() = default;

WebIDL::ExceptionOr<JS::NonnullGCPtr<DynamicsCompressorNode>> DynamicsCompressorNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, DynamicsCompressorOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-dynamicscompressornode
WebIDL::ExceptionOr<JS::NonnullGCPtr<DynamicsCompressorNode>> DynamicsCompressorNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, DynamicsCompressorOptions const& options)
{
    // Create the node and allocate memory
    auto node = realm.vm().heap().allocate<DynamicsCompressorNode>(realm, realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#DynamicsCompressorNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::ClampedMax;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to yes

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

DynamicsCompressorNode::DynamicsCompressorNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, DynamicsCompressorOptions const& options)
    : AudioNode(realm, context)
    , m_threshold(AudioParam::create(realm, options.threshold, -100, 0, Bindings::AutomationRate::KRate))
    , m_knee(AudioParam::create(realm, options.knee, 0, 40, Bindings::AutomationRate::KRate))
    , m_ratio(AudioParam::create(realm, options.ratio, 1, 20, Bindings::AutomationRate::KRate))
    , m_attack(AudioParam::create(realm, options.attack, 0, 1, Bindings::AutomationRate::KRate))
    , m_release(AudioParam::create(realm, options.release, 0, 1, Bindings::AutomationRate::KRate))
{
}

void DynamicsCompressorNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DynamicsCompressorNode);
}

void DynamicsCompressorNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_threshold);
    visitor.visit(m_knee);
    visitor.visit(m_ratio);
    visitor.visit(m_attack);
    visitor.visit(m_release);
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
WebIDL::ExceptionOr<void> DynamicsCompressorNode::set_channel_count_mode(Bindings::ChannelCountMode mode)
{
    if (mode == Bindings::ChannelCountMode::Max) {
        // Return a NotSupportedError if 'max' is used
        return WebIDL::NotSupportedError::create(realm(), "DynamicsCompressorNode does not support 'max' as channelCountMode."_string);
    }

    // If the mode is valid, call the base class implementation
    return AudioNode::set_channel_count_mode(mode);
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> DynamicsCompressorNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    if (channel_count > 2) {
        // Return a NotSupportedError if 'max' is used
        return WebIDL::NotSupportedError::create(realm(), "DynamicsCompressorNode does not support channel count greater than 2"_string);
    }

    // If the mode is valid, call the base class implementation
    return AudioNode::set_channel_count(channel_count);
}

}
