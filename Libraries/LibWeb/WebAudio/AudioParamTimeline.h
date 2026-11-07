/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Span.h>
#include <AK/Vector.h>
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
    ~AudioParamTimeline();

    // All event times passed into these methods must already be clamped to the context's current time by the caller.
    InsertResult set_value_at_time(float value, double start_time);
    InsertResult linear_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value);
    InsertResult exponential_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value);
    InsertResult set_target_at_time(float target, double start_time, float time_constant);
    InsertResult set_value_curve_at_time(Vector<float> values, double start_time, double duration);
    void cancel_scheduled_values(double cancel_time);
    void cancel_and_hold_at_time(double cancel_time);

    float value_at_time(double time) const;

    // Evaluates the intrinsic value for each sample frame of a render quantum starting at start_time.
    void sample(double start_time, double sample_period, Span<float> output) const;

    bool has_events() const;
    float default_value() const;
    void const* rust_timeline() const { return m_timeline; }

private:
    void const* m_timeline { nullptr };
};

}
