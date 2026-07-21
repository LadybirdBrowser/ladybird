/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioParam);

AudioParam::AudioParam(JS::Realm& realm, GC::Ref<BaseAudioContext> context, float default_value, float min_value, float max_value, Bindings::AutomationRate automation_rate, FixedAutomationRate fixed_automation_rate)
    : Bindings::PlatformObject(realm)
    , m_context(context)
    , m_current_value(default_value)
    , m_default_value(default_value)
    , m_min_value(min_value)
    , m_max_value(max_value)
    , m_automation_rate(automation_rate)
    , m_fixed_automation_rate(fixed_automation_rate)
{
}

GC::Ref<AudioParam> AudioParam::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, float default_value, float min_value, float max_value, Bindings::AutomationRate automation_rate, FixedAutomationRate fixed_automation_rate)
{
    return realm.create<AudioParam>(realm, context, default_value, min_value, max_value, automation_rate, fixed_automation_rate);
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
    // Setting this attribute has the effect of assigning the requested value to the [[current value]] slot, and calling
    // the setValueAtTime() method with the current AudioContext's currentTime and [[current value]].
    m_current_value = value;
    MUST(set_value_at_time(m_current_value, context()->current_time()));
}

// https://webaudio.github.io/web-audio-api/#computedvalue
float AudioParam::intrinsic_value_at_time(double time) const
{
    // paramIntrinsicValue will be calculated at each time, which is either the value set directly to the value
    // attribute, or, if there are any automation events with times before or at this time, the value as calculated from
    // these events.
    auto const& cache = parameterization_cache_for_time(time);
    if (!cache.event_index.has_value())
        return cache.starting_value;

    auto const& event = m_automation_events[*cache.event_index];
    return event.parameterization.visit(
        [](SetValue const& parameterization) {
            return parameterization.value;
        },
        [&](LinearRamp const& linear_ramp) {
            if (time >= event.time)
                return linear_ramp.value;

            VERIFY(cache.minimum_time.has_value());
            auto progress = static_cast<float>((time - *cache.minimum_time) / (event.time - *cache.minimum_time));
            return cache.starting_value + (linear_ramp.value - cache.starting_value) * progress;
        });
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
Bindings::AutomationRate AudioParam::automation_rate() const
{
    return m_automation_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-automationrate
WebIDL::ExceptionOr<void> AudioParam::set_automation_rate(Bindings::AutomationRate automation_rate)
{
    if (automation_rate != m_automation_rate && m_fixed_automation_rate == FixedAutomationRate::Yes)
        return WebIDL::InvalidStateError::create(realm(), "Automation rate cannot be changed"_utf16);

    m_automation_rate = automation_rate;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_at_time(float value, double start_time)
{
    // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"_utf16 };

    // If startTime is less than currentTime, it is clamped to currentTime.
    start_time = max(start_time, context()->current_time());

    insert_event({
        .time = start_time,
        .parameterization = SetValue { value },
    });
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dfn-automation-event
size_t AudioParam::first_event_index_after(double time) const
{
    return AK::lower_bound_index(m_automation_events, time, [](auto const& event, double time) {
        return event.time <= time ? -1 : 1;
    });
}

float AudioParam::event_value_at_time(size_t event_index, double) const
{
    auto const& event = m_automation_events[event_index];
    return event.parameterization.visit(
        [](OneOf<SetValue, LinearRamp> auto const& parameterization) {
            return parameterization.value;
        });
}

AudioParam::ParameterizationCache const& AudioParam::parameterization_cache_for_time(double time) const
{
    if (m_parameterization_cache.has_value() && m_parameterization_cache->contains(time))
        return *m_parameterization_cache;

    auto event_index = first_event_index_after(time);
    if (event_index == 0) {
        m_parameterization_cache = {
            .maximum_time = m_automation_events.is_empty() ? Optional<double> {} : m_automation_events.first().time,
            .starting_value = m_default_value,
        };
        return *m_parameterization_cache;
    }

    // A following ramp parameterization owns the interval before its event time. Other parameterizations fall through
    // to caching the preceding event below.
    if (event_index < m_automation_events.size()) {
        auto cache = m_automation_events[event_index].parameterization.visit(
            [](SetValue const&) -> Optional<ParameterizationCache> {
                return {};
            },
            [&](LinearRamp const&) -> Optional<ParameterizationCache> {
                auto const& previous_event = m_automation_events[event_index - 1];
                return ParameterizationCache {
                    .event_index = event_index,
                    .minimum_time = previous_event.time,
                    .maximum_time = m_automation_events[event_index].time,
                    .starting_value = event_value_at_time(event_index - 1, previous_event.time),
                };
            });
        if (cache.has_value()) {
            m_parameterization_cache = cache.release_value();
            return *m_parameterization_cache;
        }
    }

    auto selected_event_index = event_index - 1;
    auto const& selected_event = m_automation_events[selected_event_index];
    m_parameterization_cache = {
        .event_index = selected_event_index,
        .minimum_time = selected_event.time,
        .maximum_time = event_index < m_automation_events.size() ? Optional<double> { m_automation_events[event_index].time } : Optional<double> {},
        .starting_value = event_value_at_time(selected_event_index, selected_event.time),
    };
    return *m_parameterization_cache;
}

// https://webaudio.github.io/web-audio-api/#dfn-automation-event
void AudioParam::insert_event(AutomationEvent event)
{
    // If an event is added at a time where there are already events, it is placed after them but before later events.
    auto event_time = event.time;
    m_automation_events.insert_before_matching(move(event), [event_time](auto const& existing_event) {
        return event_time < existing_event.time;
    });
    m_parameterization_cache = {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-linearramptovalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::linear_ramp_to_value_at_time(float value, double end_time)
{
    // A RangeError exception MUST be thrown if endTime is negative or is not a finite number.
    if (end_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "endTime must not be negative"_utf16 };

    // If endTime is less than currentTime, it is clamped to currentTime.
    end_time = max(end_time, context()->current_time());

    // If there is no event preceding this event, the linear ramp behaves as if setValueAtTime(value, currentTime) were
    // called, where value is the current value of the attribute.
    if (first_event_index_after(end_time) == 0)
        MUST(set_value_at_time(m_current_value, context()->current_time()));

    insert_event({
        .time = end_time,
        .parameterization = LinearRamp { value },
    });
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-exponentialramptovalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::exponential_ramp_to_value_at_time(float value, double end_time)
{
    (void)value;
    (void)end_time;
    dbgln("FIXME: Implement AudioParam::exponential_ramp_to_value_at_time");
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-settargetattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_target_at_time(float target, double start_time, float time_constant)
{
    (void)target;
    (void)start_time;
    (void)time_constant;
    dbgln("FIXME: Implement AudioParam::set_target_at_time");
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_curve_at_time(Span<float> values, double start_time, double duration)
{
    (void)values;
    (void)start_time;
    (void)duration;
    dbgln("FIXME: Implement AudioParam::set_value_curve_at_time");
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelscheduledvalues
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::cancel_scheduled_values(double cancel_time)
{
    (void)cancel_time;
    dbgln("FIXME: Implement AudioParam::cancel_scheduled_values");
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelandholdattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::cancel_and_hold_at_time(double cancel_time)
{
    (void)cancel_time;
    dbgln("FIXME: Implement AudioParam::cancel_and_hold_at_time");
    return GC::Ref { *this };
}

void AudioParam::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioParam);
    Base::initialize(realm);
}

void AudioParam::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
}

}
