/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Math.h>
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
WebIDL::ExceptionOr<void> AudioParam::set_value(float value)
{
    // Setting this attribute has the effect of assigning the requested value to the [[current value]] slot, and calling
    // the setValueAtTime() method with the current AudioContext's currentTime and [[current value]].
    m_current_value = value;
    TRY(set_value_at_time(m_current_value, context()->current_time()));
    return {};
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
        [](OneOf<SetValue, Hold> auto const& parameterization) {
            return parameterization.value;
        },
        [&](LinearRamp const& linear_ramp) {
            if (time >= event.time)
                return linear_ramp.value;

            VERIFY(cache.minimum_time.has_value());
            auto progress = static_cast<float>((time - *cache.minimum_time) / (event.time - *cache.minimum_time));
            return cache.starting_value + (linear_ramp.value - cache.starting_value) * progress;
        },
        [&](ExponentialRamp const& exponential_ramp) {
            if (time >= event.time)
                return exponential_ramp.value;

            if (cache.starting_value == 0
                || (cache.starting_value < 0 && exponential_ramp.value > 0)
                || (cache.starting_value > 0 && exponential_ramp.value < 0))
                return cache.starting_value;

            VERIFY(cache.minimum_time.has_value());
            auto progress = static_cast<float>((time - *cache.minimum_time) / (event.time - *cache.minimum_time));
            return cache.starting_value * static_cast<float>(pow(exponential_ramp.value / cache.starting_value, progress));
        },
        [&](SetTarget const& set_target) -> float {
            if (set_target.time_constant == 0)
                return set_target.target;
            return set_target.target + (cache.starting_value - set_target.target) * static_cast<float>(exp(-(time - event.time) / set_target.time_constant));
        },
        [&](SetValueCurve const&) {
            return event_value_at_time(*cache.event_index, time);
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

    TRY(insert_event({
        .time = start_time,
        .parameterization = SetValue { value },
    }));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dfn-automation-event
size_t AudioParam::first_event_index_after(double time) const
{
    return AK::lower_bound_index(m_automation_events, time, [](auto const& event, double time) {
        return event.time <= time ? -1 : 1;
    });
}

float AudioParam::event_value_at_time(size_t event_index, double time) const
{
    auto first_event_index = event_index;
    while (first_event_index > 0 && m_automation_events[first_event_index].parameterization.has<SetTarget>())
        --first_event_index;

    auto value = m_default_value;
    for (auto current_event_index = first_event_index; current_event_index <= event_index; ++current_event_index) {
        auto const& event = m_automation_events[current_event_index];
        auto evaluation_time = current_event_index == event_index ? time : m_automation_events[current_event_index + 1].time;
        value = event.parameterization.visit(
            [](OneOf<SetValue, LinearRamp, ExponentialRamp, Hold> auto const& parameterization) {
                return parameterization.value;
            },
            [&](SetTarget const& set_target) -> float {
                if (set_target.time_constant == 0)
                    return set_target.target;
                return set_target.target + (value - set_target.target) * static_cast<float>(exp(-(evaluation_time - event.time) / set_target.time_constant));
            },
            [&](SetValueCurve const& set_value_curve) {
                if (evaluation_time >= event.time + set_value_curve.duration)
                    return set_value_curve.values.last();

                auto curve_position = (set_value_curve.values.size() - 1) * (evaluation_time - event.time) / set_value_curve.duration;
                // NB: Floating-point rounding can push curve_position to size() - 1 even though evaluation_time is
                //     still strictly less than the curve's end time, so value_index is clamped to keep value_index + 1 in bounds.
                auto value_index = min(static_cast<size_t>(floor(curve_position)), set_value_curve.values.size() - 2);
                auto interpolation_factor = static_cast<float>(curve_position - value_index);
                return set_value_curve.values[value_index]
                    + (set_value_curve.values[value_index + 1] - set_value_curve.values[value_index]) * interpolation_factor;
            });
    }
    return value;
}

// A ramp after an already-started SetTarget begins at currentTime using the target's value at that time.
Optional<AudioParam::RampStart> AudioParam::ramp_start_for_insertion_index(size_t event_index) const
{
    if (event_index == 0)
        return {};

    auto const& previous_event = m_automation_events[event_index - 1];
    auto current_time = context()->current_time();
    if (previous_event.time >= current_time || !previous_event.parameterization.has<SetTarget>())
        return {};

    return RampStart {
        .set_target_event_id = previous_event.id,
        .time = current_time,
        .value = event_value_at_time(event_index - 1, current_time),
    };
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
            [](OneOf<SetValue, SetTarget, SetValueCurve, Hold> auto const&) -> Optional<ParameterizationCache> {
                return {};
            },
            [&](OneOf<LinearRamp, ExponentialRamp> auto const& ramp) -> Optional<ParameterizationCache> {
                auto const& previous_event = m_automation_events[event_index - 1];
                double minimum_time;
                float starting_value;
                if (ramp.start.has_value() && ramp.start->set_target_event_id == previous_event.id) {
                    minimum_time = ramp.start->time;
                    starting_value = ramp.start->value;
                } else {
                    minimum_time = previous_event.parameterization.visit(
                        [&](SetValueCurve const& set_value_curve) {
                            return previous_event.time + set_value_curve.duration;
                        },
                        [&](auto const&) {
                            return previous_event.time;
                        });
                    starting_value = event_value_at_time(event_index - 1, minimum_time);
                }
                // NB: A ramp rewritten by cancelAndHoldAtTime() can end before a preceding value curve's original end.
                //     In that case, the curve owns the interval before the ramp endpoint.
                if (time < minimum_time || minimum_time >= m_automation_events[event_index].time)
                    return {};
                return ParameterizationCache {
                    .event_index = event_index,
                    .minimum_time = minimum_time,
                    .maximum_time = m_automation_events[event_index].time,
                    .starting_value = starting_value,
                };
            });
        if (cache.has_value()) {
            m_parameterization_cache = cache.release_value();
            return *m_parameterization_cache;
        }
    }

    auto selected_event_index = event_index - 1;
    auto const& selected_event = m_automation_events[selected_event_index];
    Optional<double> maximum_time;
    if (event_index < m_automation_events.size()) {
        auto const& next_event = m_automation_events[event_index];
        maximum_time = next_event.parameterization.visit(
            [&](OneOf<LinearRamp, ExponentialRamp> auto const& ramp) {
                if (ramp.start.has_value() && ramp.start->set_target_event_id == selected_event.id)
                    return ramp.start->time;
                return next_event.time;
            },
            [&](auto const&) {
                return next_event.time;
            });
    }
    m_parameterization_cache = {
        .event_index = selected_event_index,
        .minimum_time = selected_event.time,
        .maximum_time = maximum_time,
        .starting_value = event_value_at_time(selected_event_index, selected_event.time),
    };
    return *m_parameterization_cache;
}

// https://webaudio.github.io/web-audio-api/#dfn-automation-event
WebIDL::ExceptionOr<void> AudioParam::insert_event(AutomationEvent event)
{
    // If any automation method is called at a time contained in a SetValueCurve event, a NotSupportedError exception
    // MUST be thrown.
    for (size_t event_index = 0; event_index < m_automation_events.size(); ++event_index) {
        auto const& existing_event = m_automation_events[event_index];
        auto is_contained_in_curve = existing_event.parameterization.visit(
            [&](SetValueCurve const& set_value_curve) {
                auto curve_end_time = existing_event.time + set_value_curve.duration;
                if (event_index + 1 < m_automation_events.size()
                    && m_automation_events[event_index + 1].parameterization.has<Hold>()) {
                    curve_end_time = min(curve_end_time, m_automation_events[event_index + 1].time);
                }
                return event.time >= existing_event.time
                    && event.time < curve_end_time;
            },
            [](auto const&) {
                return false;
            });
        // NB: A hold event ends an active value curve without changing how the curve is sampled before the hold time.
        if (is_contained_in_curve && !event.parameterization.has<Hold>())
            return WebIDL::NotSupportedError::create(realm(), "Cannot schedule an automation event during a value curve"_utf16);
    }

    // If an event is added at a time where there are already events, it is placed after them but before later events.
    event.id = m_next_event_id++;
    auto event_time = event.time;
    m_automation_events.insert_before_matching(move(event), [event_time](auto const& existing_event) {
        return event_time < existing_event.time;
    });
    m_parameterization_cache = {};
    return {};
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
    auto event_index = first_event_index_after(end_time);
    auto ramp_start = ramp_start_for_insertion_index(event_index);
    if (event_index == 0)
        MUST(set_value_at_time(m_current_value, context()->current_time()));

    TRY(insert_event({
        .time = end_time,
        .parameterization = LinearRamp { value, ramp_start },
    }));
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
    end_time = max(end_time, context()->current_time());

    // If there is no event preceding this event, the exponential ramp behaves as if setValueAtTime(value, currentTime)
    // were called, where value is the current value of the attribute.
    auto event_index = first_event_index_after(end_time);
    auto ramp_start = ramp_start_for_insertion_index(event_index);
    if (event_index == 0)
        MUST(set_value_at_time(m_current_value, context()->current_time()));

    TRY(insert_event({
        .time = end_time,
        .parameterization = ExponentialRamp { value, ramp_start },
    }));
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

    TRY(insert_event({
        .time = start_time,
        .parameterization = SetTarget { target, time_constant },
    }));
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_curve_at_time(Span<float> values, double start_time, double duration)
{
    // An InvalidStateError exception MUST be thrown if values has a length less than 2.
    if (values.size() < 2)
        return WebIDL::InvalidStateError::create(realm(), "values must contain at least two elements"_utf16);

    // A RangeError exception MUST be thrown if startTime is negative or is not a finite number.
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"_utf16 };

    // A RangeError exception MUST be thrown if duration is not strictly positive or is not a finite number.
    if (duration <= 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "duration must be positive"_utf16 };

    // If startTime is less than currentTime, it is clamped to currentTime.
    start_time = max(start_time, context()->current_time());

    // If there are any events with a time strictly greater than startTime but strictly less than startTime + duration,
    // a NotSupportedError exception MUST be thrown.
    for (auto const& event : m_automation_events) {
        if (event.time > start_time && event.time < start_time + duration)
            return WebIDL::NotSupportedError::create(realm(), "Cannot schedule a value curve containing an automation event"_utf16);
    }

    TRY(insert_event({
        .time = start_time,
        .parameterization = SetValueCurve { Vector<float> { values }, duration },
    }));
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

    // Cancel all scheduled parameter changes with times greater than or equal to cancelTime.
    auto first_event_to_remove = AK::lower_bound_index(m_automation_events, cancel_time, [](auto const& event, double time) {
        return event.time < time ? -1 : 1;
    });

    // Any active automations whose event time is less than cancelTime are also cancelled. A SetTarget remains active
    // until the next event, while a SetValueCurve is active through the end of its duration.
    if (first_event_to_remove > 0) {
        auto const& previous_event = m_automation_events[first_event_to_remove - 1];
        auto is_active_automation = previous_event.parameterization.visit(
            [](SetTarget const&) {
                return true;
            },
            [&](SetValueCurve const& set_value_curve) {
                return cancel_time <= previous_event.time + set_value_curve.duration;
            },
            [](auto const&) {
                return false;
            });
        if (is_active_automation)
            --first_event_to_remove;
    }

    if (first_event_to_remove < m_automation_events.size())
        m_automation_events.remove(first_event_to_remove, m_automation_events.size() - first_event_to_remove);
    m_parameterization_cache = {};
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

    auto value_to_hold = intrinsic_value_at_time(cancel_time);
    auto event_index = first_event_index_after(cancel_time);
    auto rewrote_ramp = false;

    // If the next event is a ramp, rewrite it to end at cancelTime with the value from the original timeline.
    if (event_index < m_automation_events.size()) {
        auto& event = m_automation_events[event_index];
        rewrote_ramp = event.parameterization.visit(
            [&](OneOf<LinearRamp, ExponentialRamp> auto& ramp) {
                event.time = cancel_time;
                ramp.value = value_to_hold;
                return true;
            },
            [](auto&) {
                return false;
            });
    }

    if (!rewrote_ramp && event_index > 0) {
        auto const& previous_event = m_automation_events[event_index - 1];
        auto needs_hold_event = previous_event.parameterization.visit(
            [](SetTarget const&) {
                return true;
            },
            [&](SetValueCurve const& set_value_curve) {
                return cancel_time <= previous_event.time + set_value_curve.duration;
            },
            [](auto const&) {
                return false;
            });
        if (needs_hold_event) {
            TRY(insert_event({
                .time = cancel_time,
                .parameterization = Hold { value_to_hold },
            }));
        }
    }

    // Remove all events with times greater than cancelTime. The rewritten ramp or inserted hold remains at cancelTime.
    auto first_event_to_remove = first_event_index_after(cancel_time);
    if (first_event_to_remove < m_automation_events.size())
        m_automation_events.remove(first_event_to_remove, m_automation_events.size() - first_event_to_remove);
    m_parameterization_cache = {};
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
