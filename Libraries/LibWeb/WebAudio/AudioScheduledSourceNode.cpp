/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioScheduledSourceNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioScheduledSourceNode);

AudioScheduledSourceNode::AudioScheduledSourceNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context)
    : AudioNode(realm, context)
{
}

AudioScheduledSourceNode::~AudioScheduledSourceNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-onended
JS::GCPtr<WebIDL::CallbackType> AudioScheduledSourceNode::onended()
{
    return event_handler_attribute(HTML::EventNames::ended);
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-onended
void AudioScheduledSourceNode::set_onended(JS::GCPtr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::ended, value);
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-start
WebIDL::ExceptionOr<void> AudioScheduledSourceNode::start(double when)
{
    (void)when;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioScheduledSourceNode::start"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-stop
WebIDL::ExceptionOr<void> AudioScheduledSourceNode::stop(double when)
{
    (void)when;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioScheduledSourceNode::stop"_string);
}

void AudioScheduledSourceNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioScheduledSourceNode);
}

void AudioScheduledSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
