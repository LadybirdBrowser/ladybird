/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibSync/Mutex.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio {

// Holds the automation events for a single AudioParam and evaluates the parameter's intrinsic value over time.
// This timeline is shared between the control thread, which inserts events through the AudioParam interface, and
// the rendering thread, which evaluates the parameter while rendering the audio graph. All public methods lock an
// internal mutex, so both threads observe a consistent series of automation events.
// https://webaudio.github.io/web-audio-api/#dfn-automation-event
class WEB_API AudioParamTimeline final : public AtomicRefCounted<AudioParamTimeline> {
public:
    enum class [[nodiscard]] InsertResult : u8 {
        Success,
        EventContainedInValueCurve,
        ValueCurveOverlapsEvent,
    };

    explicit AudioParamTimeline(float default_value);

    // All event times passed into these methods must already be clamped to the context's current time by the caller.
    InsertResult set_value_at_time(float value, double start_time);
    InsertResult linear_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value);
    InsertResult exponential_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value);
    InsertResult set_target_at_time(float target, double start_time, float time_constant);
    InsertResult set_value_curve_at_time(Vector<float> values, double start_time, double duration);
    void cancel_scheduled_values(double cancel_time);
    void cancel_and_hold_at_time(double cancel_time);

    // https://webaudio.github.io/web-audio-api/#computedvalue
    float value_at_time(double time) const;

    // Evaluates the intrinsic value for each sample frame of a render quantum starting at start_time.
    void sample(double start_time, double sample_period, Span<float> output) const;

    bool has_events() const;
    float default_value() const { return m_default_value; }

private:
    struct SetValue {
        float value { 0 };
    };

    struct RampStart {
        size_t set_target_event_id { 0 };
        double time { 0 };
        float value { 0 };
    };

    struct LinearRamp {
        float value { 0 };
        Optional<RampStart> start;
    };

    struct ExponentialRamp {
        float value { 0 };
        Optional<RampStart> start;
    };

    struct SetTarget {
        float target { 0 };
        float time_constant { 0 };
    };

    struct SetValueCurve {
        Vector<float> values;
        double duration { 0 };
    };

    struct Hold {
        float value { 0 };
    };

    using Parameterization = Variant<SetValue, LinearRamp, ExponentialRamp, SetTarget, SetValueCurve, Hold>;

    struct AutomationEvent {
        double time { 0 };
        Parameterization parameterization;
        size_t id { 0 };
    };

    struct ParameterizationCache {
        Optional<size_t> event_index {};
        Optional<double> minimum_time {};
        Optional<double> maximum_time {};
        float starting_value { 0 };

        bool contains(double time) const
        {
            return (!minimum_time.has_value() || time >= *minimum_time)
                && (!maximum_time.has_value() || time < *maximum_time);
        }
    };

    float value_at_time_locked(double selection_time, double time) const;

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    size_t first_event_index_after(double) const;

    Optional<RampStart> ramp_start_for_insertion_index(size_t event_index, double current_time) const;

    float event_value_at_time(size_t event_index, double time) const;

    // https://webaudio.github.io/web-audio-api/#computedvalue
    ParameterizationCache const& parameterization_cache_for_time(double) const;

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    InsertResult insert_event(AutomationEvent);

    mutable Sync::Mutex m_mutex;

    float m_default_value {};

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    Vector<AutomationEvent> m_automation_events;
    size_t m_next_event_id { 0 };

    mutable Optional<ParameterizationCache> m_parameterization_cache;
};

}
