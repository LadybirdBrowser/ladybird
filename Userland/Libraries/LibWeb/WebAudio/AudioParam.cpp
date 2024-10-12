/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioParam);

AudioParam::AudioParam(JS::Realm& realm, float default_value, float min_value, float max_value, Bindings::AutomationRate automation_rate)
    : Bindings::PlatformObject(realm)
    , m_current_value(default_value)
    , m_default_value(default_value)
    , m_min_value(min_value)
    , m_max_value(max_value)
    , m_automation_rate(automation_rate)
{
}

JS::NonnullGCPtr<AudioParam> AudioParam::create(JS::Realm& realm, float default_value, float min_value, float max_value, Bindings::AutomationRate automation_rate)
{
    return realm.vm().heap().allocate<AudioParam>(realm, realm, default_value, min_value, max_value, automation_rate);
}

AudioParam::~AudioParam() = default;

// https://webaudio.github.io/web-audio-api/#dom-audioparam-value
// https://webaudio.github.io/web-audio-api/#simple-nominal-range
float AudioParam::value() const
{
    // Each AudioParam includes minValue and maxValue attributes that together form the simple nominal range
    // for the parameter. In effect, value of the parameter is clamped to the range [minValue, maxValue].
    return clamp(m_current_value, min_value(), max_value());
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-value
void AudioParam::set_value(float value)
{
    m_current_value = value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
Bindings::AutomationRate AudioParam::automation_rate() const
{
    return m_automation_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
WebIDL::ExceptionOr<void> AudioParam::set_automation_rate(Bindings::AutomationRate automation_rate)
{
    dbgln("FIXME: Fully implement AudioParam::set_automation_rate");
    m_automation_rate = automation_rate;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-defaultvalue
float AudioParam::default_value() const
{
    return m_default_value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-minvalue
float AudioParam::min_value() const
{
    return m_min_value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-maxvalue
float AudioParam::max_value() const
{
    return m_max_value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvalueattime
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::set_value_at_time(float value, double start_time)
{
    (void)value;
    (void)start_time;
    dbgln("FIXME: Implement AudioParam::set_value_at_time");
    return JS::NonnullGCPtr<AudioParam> { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-linearramptovalueattime
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::linear_ramp_to_value_at_time(float value, double end_time)
{
    (void)value;
    (void)end_time;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioParam::linear_ramp_to_value_at_time"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-exponentialramptovalueattime
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::exponential_ramp_to_value_at_time(float value, double end_time)
{
    (void)value;
    (void)end_time;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioParam::exponential_ramp_to_value_at_time"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-settargetattime
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::set_target_at_time(float target, double start_time, float time_constant)
{
    (void)target;
    (void)start_time;
    (void)time_constant;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioParam::set_target_at_time"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::set_value_curve_at_time(Span<float> values, double start_time, double duration)
{
    (void)values;
    (void)start_time;
    (void)duration;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioParam::set_value_curve_at_time"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelscheduledvalues
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::cancel_scheduled_values(double cancel_time)
{
    (void)cancel_time;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioParam::cancel_scheduled_values"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelandholdattime
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> AudioParam::cancel_and_hold_at_time(double cancel_time)
{
    (void)cancel_time;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement AudioParam::cancel_and_hold_at_time"_string);
}

void AudioParam::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioParam);
}

void AudioParam::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
