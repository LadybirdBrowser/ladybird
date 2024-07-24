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

// https://webaudio.github.io/web-audio-api/#dom-audionode-connect
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioNode>> AudioNode::connect(JS::NonnullGCPtr<AudioNode> destination_node, WebIDL::UnsignedLong output, WebIDL::UnsignedLong input)
{
    (void)output;
    (void)input;
    dbgln("FIXME: Implement AudioNode::connect (AudioNode)");
    return destination_node;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-connect-destinationparam-output
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioNode>> AudioNode::connect(JS::NonnullGCPtr<AudioParam> destination_param, WebIDL::UnsignedLong output)
{
    (void)destination_param;
    (void)output;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioNode::connect (AudioParam)"_fly_string);
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
