/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/AudioParamTimeline.h>

namespace Web::WebAudio {

AudioParamTimeline::AudioParamTimeline(float default_value)
    : m_default_value(default_value)
{
}

// https://webaudio.github.io/web-audio-api/#computedvalue
float AudioParamTimeline::value_at_time(double time) const
{
    Sync::MutexLocker locker(m_mutex);
    return value_at_time_locked(time, time);
}

float AudioParamTimeline::value_at_time_locked(double selection_time, double time) const
{
    // paramIntrinsicValue will be calculated at each time, which is either the value set directly to the value
    // attribute, or, if there are any automation events with times before or at this time, the value as calculated from
    // these events.
    // NB: The governing automation event is selected using selection_time, which render quantum sampling sets half a
    //     frame ahead of the evaluation time so events take effect at the nearest sample frame of their scheduled time
    //     rather than the next frame when floating-point rounding pushes them past the exact frame time.
    auto const& cache = parameterization_cache_for_time(selection_time);
    if (!cache.event_index.has_value())
        return cache.starting_value;

    auto const& event = m_automation_events[*cache.event_index];
    return event.parameterization.visit(
        [](OneOf<SetValue, Hold> auto const& parameterization) {
            return parameterization.value;
        },
        [&](LinearRamp const& linear_ramp) {
            // NB: When a ramp ends between two sample frames, selection can consider the ramp completed while the
            //     evaluation time still lies just before its end time. The cached minimum time is then the ramp's own
            //     end time, so interpolating would divide by zero; the ramp's end value applies instead.
            VERIFY(cache.minimum_time.has_value());
            if (time >= event.time || event.time <= *cache.minimum_time)
                return linear_ramp.value;

            auto progress = static_cast<float>((time - *cache.minimum_time) / (event.time - *cache.minimum_time));
            return cache.starting_value + (linear_ramp.value - cache.starting_value) * progress;
        },
        [&](ExponentialRamp const& exponential_ramp) {
            VERIFY(cache.minimum_time.has_value());
            if (time >= event.time || event.time <= *cache.minimum_time)
                return exponential_ramp.value;

            if (cache.starting_value == 0
                || (cache.starting_value < 0 && exponential_ramp.value > 0)
                || (cache.starting_value > 0 && exponential_ramp.value < 0))
                return cache.starting_value;

            auto progress = static_cast<float>((time - *cache.minimum_time) / (event.time - *cache.minimum_time));
            return cache.starting_value * static_cast<float>(AK::pow(exponential_ramp.value / cache.starting_value, progress));
        },
        [&](SetTarget const& set_target) -> float {
            if (set_target.time_constant == 0)
                return set_target.target;
            return set_target.target + (cache.starting_value - set_target.target) * static_cast<float>(AK::exp(-(time - event.time) / set_target.time_constant));
        },
        [&](SetValueCurve const&) {
            return event_value_at_time(*cache.event_index, time);
        });
}

// Evaluates the intrinsic value for each sample frame of a render quantum starting at start_time.
void AudioParamTimeline::sample(double start_time, double sample_period, Span<float> output) const
{
    Sync::MutexLocker locker(m_mutex);
    for (size_t frame = 0; frame < output.size(); ++frame) {
        auto frame_time = start_time + frame * sample_period;
        output[frame] = value_at_time_locked(frame_time + sample_period / 2, frame_time);
    }
}

bool AudioParamTimeline::has_events() const
{
    Sync::MutexLocker locker(m_mutex);
    return !m_automation_events.is_empty();
}

AudioParamTimeline::InsertResult AudioParamTimeline::set_value_at_time(float value, double start_time)
{
    Sync::MutexLocker locker(m_mutex);
    return insert_event({
        .time = start_time,
        .parameterization = SetValue { value },
    });
}

AudioParamTimeline::InsertResult AudioParamTimeline::linear_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value)
{
    Sync::MutexLocker locker(m_mutex);

    // If there is no event preceding this event, the linear ramp behaves as if setValueAtTime(value, currentTime) were
    // called, where value is the current value of the attribute.
    auto event_index = first_event_index_after(end_time);
    auto ramp_start = ramp_start_for_insertion_index(event_index, current_time);
    if (event_index == 0) {
        auto result = insert_event({
            .time = current_time,
            .parameterization = SetValue { current_value },
        });
        VERIFY(result == InsertResult::Success);
    }

    return insert_event({
        .time = end_time,
        .parameterization = LinearRamp { value, ramp_start },
    });
}

AudioParamTimeline::InsertResult AudioParamTimeline::exponential_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value)
{
    Sync::MutexLocker locker(m_mutex);

    // If there is no event preceding this event, the exponential ramp behaves as if setValueAtTime(value, currentTime)
    // were called, where value is the current value of the attribute.
    auto event_index = first_event_index_after(end_time);
    auto ramp_start = ramp_start_for_insertion_index(event_index, current_time);
    if (event_index == 0) {
        auto result = insert_event({
            .time = current_time,
            .parameterization = SetValue { current_value },
        });
        VERIFY(result == InsertResult::Success);
    }

    return insert_event({
        .time = end_time,
        .parameterization = ExponentialRamp { value, ramp_start },
    });
}

AudioParamTimeline::InsertResult AudioParamTimeline::set_target_at_time(float target, double start_time, float time_constant)
{
    Sync::MutexLocker locker(m_mutex);
    return insert_event({
        .time = start_time,
        .parameterization = SetTarget { target, time_constant },
    });
}

AudioParamTimeline::InsertResult AudioParamTimeline::set_value_curve_at_time(Vector<float> values, double start_time, double duration)
{
    Sync::MutexLocker locker(m_mutex);

    // If there are any events with a time strictly greater than startTime but strictly less than startTime + duration,
    // a NotSupportedError exception MUST be thrown.
    for (auto const& event : m_automation_events) {
        if (event.time > start_time && event.time < start_time + duration)
            return InsertResult::ValueCurveOverlapsEvent;
    }

    return insert_event({
        .time = start_time,
        .parameterization = SetValueCurve { move(values), duration },
    });
}

// https://webaudio.github.io/web-audio-api/#dfn-automation-event
size_t AudioParamTimeline::first_event_index_after(double time) const
{
    return AK::lower_bound_index(m_automation_events, time, [](auto const& event, double time) {
        return event.time <= time ? -1 : 1;
    });
}

float AudioParamTimeline::event_value_at_time(size_t event_index, double time) const
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
                return set_target.target + (value - set_target.target) * static_cast<float>(AK::exp(-(evaluation_time - event.time) / set_target.time_constant));
            },
            [&](SetValueCurve const& set_value_curve) {
                if (evaluation_time >= event.time + set_value_curve.duration)
                    return set_value_curve.values.last();

                auto curve_position = (set_value_curve.values.size() - 1) * (evaluation_time - event.time) / set_value_curve.duration;

                // NB: Floating-point rounding can push curve_position to size() - 1 even though evaluation_time is
                //     still strictly less than the curve's end time, so value_index is clamped to keep value_index + 1 in bounds.
                auto value_index = min(static_cast<size_t>(AK::floor(curve_position)), set_value_curve.values.size() - 2);
                auto interpolation_factor = static_cast<float>(curve_position - value_index);
                return set_value_curve.values[value_index]
                    + (set_value_curve.values[value_index + 1] - set_value_curve.values[value_index]) * interpolation_factor;
            });
    }
    return value;
}

// A ramp after an already-started SetTarget begins at currentTime using the target's value at that time.
Optional<AudioParamTimeline::RampStart> AudioParamTimeline::ramp_start_for_insertion_index(size_t event_index, double current_time) const
{
    if (event_index == 0)
        return {};

    auto const& previous_event = m_automation_events[event_index - 1];
    if (previous_event.time >= current_time || !previous_event.parameterization.has<SetTarget>())
        return {};

    return RampStart {
        .set_target_event_id = previous_event.id,
        .time = current_time,
        .value = event_value_at_time(event_index - 1, current_time),
    };
}

AudioParamTimeline::ParameterizationCache const& AudioParamTimeline::parameterization_cache_for_time(double time) const
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
AudioParamTimeline::InsertResult AudioParamTimeline::insert_event(AutomationEvent event)
{
    auto insertion_index = first_event_index_after(event.time);

    // If any automation method is called at a time contained in a SetValueCurve event, a NotSupportedError exception
    // MUST be thrown.
    if (insertion_index > 0) {
        auto const& previous_event = m_automation_events[insertion_index - 1];
        auto is_contained_in_curve = previous_event.parameterization.visit(
            [&](SetValueCurve const& set_value_curve) {
                return event.time < previous_event.time + set_value_curve.duration;
            },
            [](auto const&) {
                return false;
            });

        // NB: A hold event ends an active value curve without changing how the curve is sampled before the hold time.
        if (is_contained_in_curve && !event.parameterization.has<Hold>())
            return InsertResult::EventContainedInValueCurve;
    }

    // If an event is added at a time where there are already events, it is placed after them but before later events.
    event.id = m_next_event_id++;
    m_automation_events.insert(insertion_index, move(event));
    m_parameterization_cache = {};
    return InsertResult::Success;
}

void AudioParamTimeline::cancel_scheduled_values(double cancel_time)
{
    Sync::MutexLocker locker(m_mutex);

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
}

void AudioParamTimeline::cancel_and_hold_at_time(double cancel_time)
{
    Sync::MutexLocker locker(m_mutex);

    auto value_to_hold = value_at_time_locked(cancel_time, cancel_time);
    auto event_index = first_event_index_after(cancel_time);

    // If the event immediately after cancelTime is a ramp, rewrite it to end at cancelTime with the value from the
    // original timeline. This takes precedence over holding the value of the event active at cancelTime.
    auto rewrote_ramp = false;
    if (event_index < m_automation_events.size()) {
        auto& next_event = m_automation_events[event_index];
        rewrote_ramp = next_event.parameterization.visit(
            [&](OneOf<LinearRamp, ExponentialRamp> auto& ramp) {
                next_event.time = cancel_time;
                ramp.value = value_to_hold;
                return true;
            },
            [](auto&) {
                return false;
            });
    }

    // Otherwise, decide which value (if any) to hold from cancelTime onward, based on the event active at cancelTime.
    Optional<float> hold_value;
    auto drop_active_curve = false;
    if (!rewrote_ramp && event_index > 0) {
        auto const& active_event = m_automation_events[event_index - 1];
        hold_value = active_event.parameterization.visit(
            [&](SetTarget const&) -> Optional<float> {
                return value_to_hold;
            },
            [&](SetValueCurve const& set_value_curve) -> Optional<float> {
                // Cancelling exactly at a curve's start truncates it to a zero-duration curve, which produces no
                // output, so the value reverts to the value before the curve rather than the curve's first element.
                if (active_event.time == cancel_time) {
                    drop_active_curve = true;
                    return event_index >= 2 ? event_value_at_time(event_index - 2, cancel_time) : m_default_value;
                }
                if (cancel_time <= active_event.time + set_value_curve.duration)
                    return value_to_hold;
                return {};
            },
            [](auto const&) -> Optional<float> {
                return {};
            });
    }

    // Curve removal and hold insertion mutate the event list, so they run after the visit rather than inside it.
    if (drop_active_curve)
        m_automation_events.remove(event_index - 1);
    if (hold_value.has_value())
        VERIFY(insert_event({ .time = cancel_time, .parameterization = Hold { *hold_value } }) == InsertResult::Success);

    // Remove all events with times greater than cancelTime. The rewritten ramp or inserted hold remains at cancelTime.
    auto first_event_to_remove = first_event_index_after(cancel_time);
    if (first_event_to_remove < m_automation_events.size())
        m_automation_events.remove(first_event_to_remove, m_automation_events.size() - first_event_to_remove);
    m_parameterization_cache = {};
}

}
