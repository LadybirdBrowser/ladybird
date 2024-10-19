/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioNode);

AudioNode::AudioNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context)
    : DOM::EventTarget(realm)
    , m_context(context)

{
}

AudioNode::~AudioNode() = default;

WebIDL::ExceptionOr<void> AudioNode::initialize_audio_node_options(AudioNodeOptions const& given_options, AudioNodeDefaultOptions const& default_options)
{
    // Set channel count, fallback to default if not provided
    if (given_options.channel_count.has_value()) {
        TRY(set_channel_count(given_options.channel_count.value()));
    } else {
        TRY(set_channel_count(default_options.channel_count));
    }

    // Set channel count mode, fallback to default if not provided
    if (given_options.channel_count_mode.has_value()) {
        TRY(set_channel_count_mode(given_options.channel_count_mode.value()));
    } else {
        TRY(set_channel_count_mode(default_options.channel_count_mode));
    }

    // Set channel interpretation, fallback to default if not provided
    if (given_options.channel_interpretation.has_value()) {
        TRY(set_channel_interpretation(given_options.channel_interpretation.value()));
    } else {
        TRY(set_channel_interpretation(default_options.channel_interpretation));
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-connect
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioNode>> AudioNode::connect(JS::NonnullGCPtr<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input)
{
    // There can only be one connection between a given output of one specific node and a given input of another specific node.
    // Multiple connections with the same termini are ignored.

    // If the destination parameter is an AudioNode that has been created using another AudioContext, an InvalidAccessError MUST be thrown.
    if (m_context != destination_node->m_context) {
        return WebIDL::InvalidAccessError::create(realm(), "Cannot connect to an AudioNode in a different AudioContext"_string);
    }

    (void)output;
    (void)input;
    dbgln("FIXME: Implement Audio::connect(AudioNode)");
    return destination_node;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-connect-destinationparam-output
void AudioNode::connect(JS::NonnullGCPtr<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    (void)destination_param;
    (void)output;
    dbgln("FIXME: Implement AudioNode::connect(AudioParam)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect
void AudioNode::disconnect()
{
    dbgln("FIXME: Implement AudioNode::disconnect()");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-output
void AudioNode::disconnect(WebIDL::UnsignedLong output)
{
    (void)output;
    dbgln("FIXME: Implement AudioNode::disconnect(output)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode
void AudioNode::disconnect(JS::NonnullGCPtr<AudioNode> destination_node)
{
    (void)destination_node;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_node)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output
void AudioNode::disconnect(JS::NonnullGCPtr<AudioNode> destination_node, WebIDL::UnsignedLong output)
{
    (void)destination_node;
    (void)output;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_node, output)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output-input
void AudioNode::disconnect(JS::NonnullGCPtr<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input)
{
    (void)destination_node;
    (void)output;
    (void)input;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_node, output, input)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam
void AudioNode::disconnect(JS::NonnullGCPtr<AudioParam> destination_param)
{
    (void)destination_param;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_param)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam-output
void AudioNode::disconnect(JS::NonnullGCPtr<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    (void)destination_param;
    (void)output;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_param, output)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> AudioNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // If this value is set to zero or to a value greater than the implementation’s maximum number
    // of channels the implementation MUST throw a NotSupportedError exception.
    if (channel_count == 0 || channel_count > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm(), "Invalid channel count"_string);

    m_channel_count = channel_count;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
WebIDL::ExceptionOr<void> AudioNode::set_channel_count_mode(Bindings::ChannelCountMode channel_count_mode)
{
    m_channel_count_mode = channel_count_mode;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
Bindings::ChannelCountMode AudioNode::channel_count_mode()
{
    return m_channel_count_mode;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelinterpretation
WebIDL::ExceptionOr<void> AudioNode::set_channel_interpretation(Bindings::ChannelInterpretation channel_interpretation)
{
    m_channel_interpretation = channel_interpretation;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelinterpretation
Bindings::ChannelInterpretation AudioNode::channel_interpretation()
{
    return m_channel_interpretation;
}

void AudioNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioNode);
}

void AudioNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
