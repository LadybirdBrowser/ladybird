/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
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
    , m_node_id(context->next_node_id({}))

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
    AudioNodeConnection output_connection { destination_node, output, input };
    AudioNodeConnection input_connection { *this, output, input };

    // There can only be one connection between a given output of one specific node and a given input of another specific node.
    // Multiple connections with the same termini are ignored.
    for (auto const& existing_connection : m_output_connections) {
        if (existing_connection == output_connection)
            return destination_node;
    }

    // If the destination parameter is an AudioNode that has been created using another AudioContext, an InvalidAccessError MUST be thrown.
    if (m_context != destination_node->m_context) {
        return WebIDL::InvalidAccessError::create(realm(), "Cannot connect to an AudioNode in a different AudioContext"_utf16);
    }

    // The output parameter is an index describing which output of the AudioNode from which to connect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Output index {} exceeds number of outputs", output));
    }

    // The input parameter is an index describing which input of the destination AudioNode to connect to.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (input >= destination_node->number_of_inputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Input index '{}' exceeds number of inputs", input));
    }
    // Connect node's output to destination_node input.
    m_output_connections.append(output_connection);
    // Connect destination_node input to node's output.
    destination_node->m_input_connections.append(input_connection);

    return destination_node;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-connect-destinationparam-output
WebIDL::ExceptionOr<void> AudioNode::connect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    AudioParamConnection param_connection { destination_param, output };

    // There can only be one connection between a given output of one specific node and a specific AudioParam. Multiple connections
    //  with the same termini are ignored.
    for (auto const& existing_connection : m_param_connections) {
        if (existing_connection == param_connection)
            return {};
    }

    // If destinationParam belongs to an AudioNode that belongs to a BaseAudioContext that is different from the BaseAudioContext
    // that has created the AudioNode on which this method was called, an InvalidAccessError MUST be thrown.
    if (m_context != destination_param->context()) {
        return WebIDL::InvalidAccessError::create(realm(), "Cannot connect to an AudioParam in a different AudioContext"_utf16);
    }

    // The output parameter is an index describing which output of the AudioNode from which to connect.
    // If the parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Output index {} exceeds number of outputs", output));
    }

    // Connect node's output to destination_param.
    m_param_connections.append(param_connection);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect
void AudioNode::disconnect()
{
    while (!m_output_connections.is_empty()) {
        auto connection = m_output_connections.take_last();
        auto destination = connection.destination_node;

        destination->m_input_connections.remove_all_matching([&](AudioNodeConnection& input_connection) {
            return input_connection.destination_node.ptr() == this;
        });
    }

    m_param_connections.clear();
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-output
WebIDL::ExceptionOr<void> AudioNode::disconnect(WebIDL::UnsignedLong output)
{
    // The output parameter is an index describing which output of the AudioNode to disconnect.
    // It disconnects all outgoing connections from the given output.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Output index {} exceeds number of outputs", output));
    }

    m_output_connections.remove_all_matching([&](AudioNodeConnection& connection) {
        if (connection.output != output)
            return false;

        connection.destination_node->m_input_connections.remove_all_matching([&](AudioNodeConnection& reverse_connection) {
            return reverse_connection.destination_node.ptr() == this && reverse_connection.output == output;
        });

        return true;
    });

    m_param_connections.remove_all_matching([&](AudioParamConnection& connection) {
        return connection.output == output;
    });

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioNode> destination_node)
{
    // The destinationNode parameter is the AudioNode to disconnect.
    // It disconnects all outgoing connections to the given destinationNode.
    auto before = m_output_connections.size();
    m_output_connections.remove_all_matching([&](AudioNodeConnection& connection) {
        if (connection.destination_node != destination_node)
            return false;

        connection.destination_node->m_input_connections.remove_all_matching([&](AudioNodeConnection& reverse_connection) {
            return reverse_connection.destination_node.ptr() == this;
        });

        return true;
    });
    // If there is no connection to the destinationNode, an InvalidAccessError exception MUST be thrown.
    if (m_output_connections.size() == before) {
        return WebIDL::InvalidAccessError::create(realm(), Utf16String::formatted("No connection to given AudioNode"));
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output)
{
    // The output parameter is an index describing which output of the AudioNode from which to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Output index {} exceeds number of outputs", output));
    }

    // The destinationNode parameter is the AudioNode to disconnect.
    auto before = m_output_connections.size();
    m_output_connections.remove_all_matching([&](AudioNodeConnection& connection) {
        if (connection.destination_node != destination_node || connection.output != output)
            return false;

        connection.destination_node->m_input_connections.remove_all_matching([&](AudioNodeConnection& reverse_connection) {
            return reverse_connection.destination_node.ptr() == this && reverse_connection.output == output;
        });

        return true;
    });

    //  If there is no connection to the destinationNode from the given output, an InvalidAccessError exception MUST be thrown.
    if (m_output_connections.size() == before) {
        return WebIDL::InvalidAccessError::create(realm(), Utf16String::formatted("No connection from output {} to given AudioNode", output));
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationnode-output-input
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input)
{
    // The output parameter is an index describing which output of the AudioNode from which to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Output index {} exceeds number of outputs", output));
    }

    // The input parameter is an index describing which input of the destination AudioNode to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (input >= destination_node->number_of_inputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Input index '{}' exceeds number of inputs", input));
    }

    // The destinationNode parameter is the AudioNode to disconnect.
    auto before = m_output_connections.size();
    m_output_connections.remove_all_matching([&](AudioNodeConnection& connection) {
        if (connection.destination_node != destination_node || connection.output != output || connection.input != input)
            return false;

        connection.destination_node->m_input_connections.remove_all_matching([&](AudioNodeConnection& reverse_connection) {
            return reverse_connection.destination_node.ptr() == this && reverse_connection.output == output && reverse_connection.input == input;
        });

        return true;
    });

    // If there is no connection to the destinationNode from the given output to the given input, an InvalidAccessError exception MUST be thrown.
    if (m_output_connections.size() == before) {
        return WebIDL::InvalidAccessError::create(realm(), Utf16String::formatted("No connection from output {} to input {} of given AudioNode", output, input));
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioParam> destination_param)
{
    // The destinationParam parameter is the AudioParam to disconnect.
    auto before = m_param_connections.size();
    m_param_connections.remove_all_matching([&](AudioParamConnection& connection) {
        return connection.destination_param == destination_param;
    });

    // If there is no connection to the destinationParam, an InvalidAccessError exception MUST be thrown.
    if (m_param_connections.size() == before) {
        return WebIDL::InvalidAccessError::create(realm(), Utf16String::formatted("No connection to given AudioParam"));
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-disconnect-destinationparam-output
WebIDL::ExceptionOr<void> AudioNode::disconnect(GC::Ref<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    // The output parameter is an index describing which output of the AudioNode from which to disconnect.
    // If this parameter is out-of-bounds, an IndexSizeError exception MUST be thrown.
    if (output >= number_of_outputs()) {
        return WebIDL::IndexSizeError::create(realm(), Utf16String::formatted("Output index {} exceeds number of outputs", output));
    }
    // The destinationParam parameter is the AudioParam to disconnect.
    auto before = m_param_connections.size();
    m_param_connections.remove_all_matching([&](AudioParamConnection& connection) {
        return connection.destination_param == destination_param && connection.output == output;
    });

    // If there is no connection to the destinationParam, an InvalidAccessError exception MUST be thrown.
    if (m_param_connections.size() == before) {
        return WebIDL::InvalidAccessError::create(realm(), Utf16String::formatted("No connection from output {} to given AudioParam", output));
    }

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> AudioNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // If this value is set to zero or to a value greater than the implementation’s maximum number
    // of channels the implementation MUST throw a NotSupportedError exception.
    if (channel_count == 0 || channel_count > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm(), "Invalid channel count"_utf16);

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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioNode);
    Base::initialize(realm);
}

void AudioNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    for (auto& conn : m_param_connections)
        visitor.visit(conn.destination_param);

    for (auto& conn : m_input_connections)
        visitor.visit(conn.destination_node);

    for (auto& conn : m_output_connections)
        visitor.visit(conn.destination_node);
}

}
