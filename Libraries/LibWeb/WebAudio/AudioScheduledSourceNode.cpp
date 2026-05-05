/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioScheduledSourceNode.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ControlMessage.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioScheduledSourceNode);

AudioScheduledSourceNode::AudioScheduledSourceNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context)
    : AudioNode(realm, context)
{
}

AudioScheduledSourceNode::~AudioScheduledSourceNode() = default;

void AudioScheduledSourceNode::latch_start_time_for_rendering(f64 requested_when)
{
    m_effective_start_when = effective_time_for_control_thread_scheduling(requested_when);
}

void AudioScheduledSourceNode::clear_stop_time_for_rendering()
{
    m_effective_stop_when = {};
}

void AudioScheduledSourceNode::latch_stop_time_for_rendering(f64 requested_when)
{
    double const effective_stop_when = effective_time_for_control_thread_scheduling(requested_when);
    if (!m_effective_stop_when.has_value() || effective_stop_when < m_effective_stop_when.value())
        m_effective_stop_when = effective_stop_when;
}

f64 AudioScheduledSourceNode::effective_time_for_control_thread_scheduling(f64 requested_when) const
{
    double const current_time = context()->current_time();

    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-start
    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-stop
    // The when parameter describes at what time the source should start or stop playing.
    // If 0 is passed in for this value or if the value is less than currentTime, then the sound will start or stop immediately.
    if (requested_when == 0.0 || requested_when < current_time)
        return current_time;

    return requested_when;
}

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
        return WebIDL::InvalidStateError::create(realm(), "AudioScheduledSourceNode source has already started"_utf16);

    // 2. Check for any errors that must be thrown due to parameter constraints described below.
    //    If any exception is thrown during this step, abort those steps.
    //    A RangeError exception MUST be thrown if when is negative.
    if (when < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "when must not be negative"sv };

    // 3. Set the internal slot [[source started]] on this AudioScheduledSourceNode to true.
    set_source_started(true);

    m_start_when = when;
    m_stop_when = {};
    latch_start_time_for_rendering(when);
    clear_stop_time_for_rendering();

    // 4. Queue a control message to start the AudioScheduledSourceNode, including the parameter values in the message.
    context()->queue_control_message(StartSource { .node_id = node_id(), .when = when });

    context()->notify_audio_graph_changed();

    // FIXME: 5. Send a control message to the associated AudioContext to start running its rendering thread only when all the following conditions are met:
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-stop
WebIDL::ExceptionOr<void> AudioScheduledSourceNode::stop(double when)
{
    // 1. If this AudioScheduledSourceNode internal slot [[source started]] is not true, an InvalidStateError exception MUST be thrown.
    if (!m_source_started)
        return WebIDL::InvalidStateError::create(realm(), "AudioScheduledSourceNode source has not been started"_utf16);

    // 2. Check for any errors that must be thrown due to parameter constraints described below.
    //    A RangeError exception MUST be thrown if when is negative.
    if (when < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "when must not be negative"sv };

    // 3. Queue a control message to stop the AudioScheduledSourceNode, including the parameter values in the message.
    context()->queue_control_message(StopSource { .node_id = node_id(), .when = when });

    if (!m_stop_when.has_value() || when < m_stop_when.value())
        m_stop_when = when;

    latch_stop_time_for_rendering(when);

    context()->notify_audio_graph_changed();

    double const effective_stop_when = m_effective_stop_when.value();
    context()->schedule_source_end(*this, effective_stop_when);

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
