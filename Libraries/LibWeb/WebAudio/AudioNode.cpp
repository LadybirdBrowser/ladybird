/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioNode);

AudioNode::AudioNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, WebIDL::UnsignedLong channel_count)
    : DOM::EventTarget(realm)
    , m_context(context)
    , m_channel_count(channel_count)

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
WebIDL::ExceptionOr<GC::Ref<AudioNode>> AudioNode::connect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input)
{
    // There can only be one connection between a given output of one specific node and a given input of another specific node.
    // Multiple connections with the same termini are ignored.

    // If the destination parameter is an AudioNode that has been created using another AudioContext, an InvalidAccessError MUST be thrown.
    if (m_context != destination_node->m_context) {
        return WebIDL::InvalidAccessError::create(realm(), "Cannot connect to an AudioNode in a different AudioContext"_string);
    }

    // The output parameter is an index describing which output of the AudioNode from which to connect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Output index {} exceeds number of outputs", output)));
    }

    // The input parameter is an index describing which input of the destination AudioNode to connect to.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (input >= destination_node->number_of_inputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Input index '{}' exceeds number of inputs", input)));
    }

    dbgln("FIXME: Implement Audio::connect(AudioNode)");
    return destination_node;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-connect-destinationparam-output
WebIDL::ExceptionOr<void> AudioNode::connect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    // If destinationParam belongs to an AudioNode that belongs to a BaseAudioContext that is different from the BaseAudioContext
    // that has created the AudioNode on which this method was called, an InvalidAccessError MUST be thrown.
    if (m_context != destination_param->context()) {
        return WebIDL::InvalidAccessError::create(realm(), "Cannot connect to an AudioParam in a different AudioContext"_string);
    }

    // The output parameter is an index describing which output of the AudioNode from which to connect.
    // If the parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Output index {} exceeds number of outputs", output)));
    }

    dbgln("FIXME: Implement AudioNode::connect(AudioParam)");
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect
void AudioNode::disconnect()
{
    dbgln("FIXME: Implement AudioNode::disconnect()");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-output
WebIDL::ExceptionOr<void> AudioNode::disconnect(WebIDL::UnsignedLong output)
{
    // The output parameter is an index describing which output of the AudioNode to disconnect.
    // It disconnects all outgoing connections from the given output.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Output index {} exceeds number of outputs", output)));
    }

    dbgln("FIXME: Implement AudioNode::disconnect(output)");
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode
void AudioNode::disconnect(GC::Ref<AudioNode> destination_node)
{
    (void)destination_node;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_node)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output)
{
    (void)destination_node;
    // The output parameter is an index describing which output of the AudioNode from which to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Output index {} exceeds number of outputs", output)));
    }

    dbgln("FIXME: Implement AudioNode::disconnect(destination_node, output)");
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output-input
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input)
{
    (void)destination_node;
    // The output parameter is an index describing which output of the AudioNode from which to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Output index {} exceeds number of outputs", output)));
    }

    // The input parameter is an index describing which input of the destination AudioNode to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (input >= destination_node->number_of_inputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Input index '{}' exceeds number of inputs", input)));
    }

    dbgln("FIXME: Implement AudioNode::disconnect(destination_node, output, input)");
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam
void AudioNode::disconnect(GC::Ref<AudioParam> destination_param)
{
    (void)destination_param;
    dbgln("FIXME: Implement AudioNode::disconnect(destination_param)");
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam-output
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    (void)destination_param;
    // The output parameter is an index describing which output of the AudioNode from which to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), MUST(String::formatted("Output index {} exceeds number of outputs", output)));
    }

    dbgln("FIXME: Implement AudioNode::disconnect(destination_param, output)");
    return {};
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
