/*
 * Copyright (c) 2025, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ScriptProcessorNodePrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ScriptProcessorNode);

ScriptProcessorNode::~ScriptProcessorNode() = default;

WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> ScriptProcessorNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context,
    WebIDL::UnsignedLong buffer_size, WebIDL::UnsignedLong number_of_input_channels, WebIDL::UnsignedLong number_of_output_channels)
{
    auto node = realm.create<ScriptProcessorNode>(realm, context, buffer_size, number_of_input_channels, number_of_output_channels);

    // This is the number of channels specified when constructing this node. There are channelCount constraints.
    // FIXME: Implement channel count constraints: https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
    TRY(node->set_channel_count(number_of_input_channels));
    // Has channelCountMode constraints
    // FIXME: Implement channel count mode constraints: https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
    TRY(node->set_channel_count_mode(Bindings::ChannelCountMode::Explicit));
    TRY(node->set_channel_interpretation(Bindings::ChannelInterpretation::Speakers));

    return node;
}

ScriptProcessorNode::ScriptProcessorNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context,
    WebIDL::UnsignedLong buffer_size, WebIDL::UnsignedLong number_of_input_channels, WebIDL::UnsignedLong number_of_output_channels)
    : AudioNode(realm, context)
    , m_buffer_size(buffer_size)
    , m_number_of_input_channels(number_of_input_channels)
    , m_number_of_output_channels(number_of_output_channels)
{
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

void ScriptProcessorNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ScriptProcessorNode);
}

}
