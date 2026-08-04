/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioParam);

AudioParam::AudioParam(GC::Ref<BaseAudioContext> context, GC::Ptr<AudioNode> owner, float default_value, float min_value, float max_value, AutomationRate automation_rate, FixedAutomationRate fixed_automation_rate)
    : m_context(context)
    , m_owner(owner)
    , m_timeline(adopt_ref(*new AudioParamTimeline(default_value)))
    , m_render_param(adopt_ref(*new Rendering::RenderAudioParam(m_timeline, min_value, max_value, automation_rate)))
    , m_current_value(default_value)
    , m_default_value(default_value)
    , m_min_value(min_value)
    , m_max_value(max_value)
    , m_automation_rate(automation_rate)
    , m_fixed_automation_rate(fixed_automation_rate)
{
}

GC::Ref<AudioParam> AudioParam::create(GC::Ref<BaseAudioContext> context, GC::Ptr<AudioNode> owner, float default_value, float min_value, float max_value, AutomationRate automation_rate, FixedAutomationRate fixed_automation_rate)
{
    return GC::Heap::the().allocate<AudioParam>(context, owner, default_value, min_value, max_value, automation_rate, fixed_automation_rate);
}

AudioParam::~AudioParam() = default;

// https://webaudio.github.io/web-audio-api/#dom-audioparam-value
// https://webaudio.github.io/web-audio-api/#simple-nominal-range
float AudioParam::value() const
{
    // Each AudioParam includes minValue and maxValue attributes that together form the simple nominal range
    // for the parameter. In effect, value of the parameter is clamped to the range [minValue, maxValue].
    if (m_timeline->has_events())
        return clamp(m_timeline->value_at_time(context()->current_time()), min_value(), max_value());
    return clamp(m_current_value, min_value(), max_value());
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-value
WebIDL::ExceptionOr<void> AudioParam::set_value(float value)
{
    // Setting this attribute has the effect of assigning the requested value to the [[current value]] slot, and calling
    // the setValueAtTime() method with the current AudioContext's currentTime and [[current value]].
    m_current_value = value;
    TRY(set_value_at_time(m_current_value, context()->current_time()));
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
AutomationRate AudioParam::automation_rate() const
{
    return m_automation_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
WebIDL::ExceptionOr<void> AudioParam::set_automation_rate(AutomationRate automation_rate)
{
    if (automation_rate != m_automation_rate && m_fixed_automation_rate == FixedAutomationRate::Yes)
        return WebIDL::InvalidStateError::create("Automation rate cannot be changed"_utf16);

    m_automation_rate = automation_rate;
    m_render_param->set_automation_rate(automation_rate);
    return {};
}

WebIDL::ExceptionOr<void> AudioParam::map_insert_result(AudioParamTimeline::InsertResult result)
{
    switch (result) {
    case AudioParamTimeline::InsertResult::Success:
        return {};
    case AudioParamTimeline::InsertResult::EventContainedInValueCurve:
        return WebIDL::NotSupportedError::create(context()->relevant_settings_object().realm(), "Cannot schedule an automation event during a value curve"_utf16);
    case AudioParamTimeline::InsertResult::ValueCurveOverlapsEvent:
        return WebIDL::NotSupportedError::create(context()->relevant_settings_object().realm(), "Cannot schedule a value curve containing an automation event"_utf16);
    }
    VERIFY_NOT_REACHED();
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_at_time(float value, double start_time)
{
    // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"_utf16 };

    // If startTime is less than currentTime, it is clamped to currentTime.
    start_time = max(start_time, context()->current_time());

    TRY(map_insert_result(m_timeline->set_value_at_time(value, start_time)));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-linearramptovalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::linear_ramp_to_value_at_time(float value, double end_time)
{
    // A RangeError exception MUST be thrown if endTime is negative or is not a finite number.
    if (end_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "endTime must not be negative"_utf16 };

    // If endTime is less than currentTime, it is clamped to currentTime.
    auto current_time = context()->current_time();
    end_time = max(end_time, current_time);

    TRY(map_insert_result(m_timeline->linear_ramp_to_value_at_time(value, end_time, current_time, m_current_value)));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-exponentialramptovalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::exponential_ramp_to_value_at_time(float value, double end_time)
{
    // A RangeError exception MUST be thrown if value is equal to 0.
    if (value == 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "value must not be zero"_utf16 };

    // A RangeError exception MUST be thrown if endTime is negative or is not a finite number.
    if (end_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "endTime must not be negative"_utf16 };

    // If endTime is less than currentTime, it is clamped to currentTime.
    auto current_time = context()->current_time();
    end_time = max(end_time, current_time);

    TRY(map_insert_result(m_timeline->exponential_ramp_to_value_at_time(value, end_time, current_time, m_current_value)));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-settargetattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_target_at_time(float target, double start_time, float time_constant)
{
    // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"_utf16 };

    // A RangeError exception MUST be thrown if timeConstant is negative or is not a finite number.
    if (time_constant < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "timeConstant must not be negative"_utf16 };

    // If startTime is less than currentTime, it is clamped to currentTime.
    start_time = max(start_time, context()->current_time());

    TRY(map_insert_result(m_timeline->set_target_at_time(target, start_time, time_constant)));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_curve_at_time(Span<float> values, double start_time, double duration)
{
    // An InvalidStateError exception MUST be thrown if values has a length less than 2.
    if (values.size() < 2)
        return WebIDL::InvalidStateError::create(context()->relevant_settings_object().realm(), "values must contain at least two elements"_utf16);

    // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"_utf16 };

    // A RangeError exception MUST be thrown if duration is not strictly positive or is not a finite number.
    if (duration <= 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "duration must be positive"_utf16 };

    // If startTime is less than currentTime, it is clamped to currentTime.
    start_time = max(start_time, context()->current_time());

    TRY(map_insert_result(m_timeline->set_value_curve_at_time(Vector<float> { values }, start_time, duration)));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelscheduledvalues
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::cancel_scheduled_values(double cancel_time)
{
    // A RangeError exception MUST be thrown if cancelTime is negative.
    if (cancel_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "cancelTime must not be negative"_utf16 };

    // If cancelTime is less than currentTime, it is clamped to currentTime.
    cancel_time = max(cancel_time, context()->current_time());

    m_timeline->cancel_scheduled_values(cancel_time);
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelandholdattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::cancel_and_hold_at_time(double cancel_time)
{
    // A RangeError exception MUST be thrown if cancelTime is negative.
    if (cancel_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "cancelTime must not be negative"_utf16 };

    // If cancelTime is less than currentTime, it is clamped to currentTime.
    cancel_time = max(cancel_time, context()->current_time());

    m_timeline->cancel_and_hold_at_time(cancel_time);
    return GC::Ref { *this };
}

void AudioParam::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    visitor.visit(m_owner);
}

}
