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

GC_DEFINE_ALLOCATOR(AudioScheduledSourceNode);

AudioScheduledSourceNode::AudioScheduledSourceNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context)
    : AudioNode(realm, context)
{
}

AudioScheduledSourceNode::~AudioScheduledSourceNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-onended
GC::Ptr<WebIDL::CallbackType> AudioScheduledSourceNode::onended()
{
    return event_handler_attribute(HTML::EventNames::ended);
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-onended
void AudioScheduledSourceNode::set_onended(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::ended, value);
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-start
WebIDL::ExceptionOr<void> AudioScheduledSourceNode::start(double when)
{
    // 1. If this AudioScheduledSourceNode internal slot [[source started]] is true, an InvalidStateError exception MUST be thrown.
    if (source_started())
        return WebIDL::InvalidStateError::create(realm(), "AudioScheduledSourceNode source has already started"_string);

    // 2. Check for any errors that must be thrown due to parameter constraints described below. If any exception is thrown during this step, abort those steps.
    // A RangeError exception MUST be thrown if when is negative.
    if (when < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "when must not be negative"sv };

    // 3. Set the internal slot [[source started]] on this AudioScheduledSourceNode to true.
    set_source_started(true);

    // FIXME: 4. Queue a control message to start the AudioScheduledSourceNode, including the parameter values in the message.
    // FIXME: 5. Send a control message to the associated AudioContext to start running its rendering thread only when all the following conditions are met:

    dbgln("FIXME: Implement AudioScheduledSourceNode::start");
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-stop
WebIDL::ExceptionOr<void> AudioScheduledSourceNode::stop(double when)
{
    // 1. If this AudioScheduledSourceNode internal slot [[source started]] is not true, an InvalidStateError exception MUST be thrown.
    if (!m_source_started)
        return WebIDL::InvalidStateError::create(realm(), "AudioScheduledSourceNode source has not been started"_string);

    // 2. Check for any errors that must be thrown due to parameter constraints described below.
    // A RangeError exception MUST be thrown if when is negative.
    if (when < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "when must not be negative"sv };

    // FIXME: 3. Queue a control message to stop the AudioScheduledSourceNode, including the parameter values in the message.

    dbgln("FIXME: Implement AudioScheduledSourceNode::stop");
    return {};
}

void AudioScheduledSourceNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioScheduledSourceNode);
    Base::initialize(realm);
}

void AudioScheduledSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
