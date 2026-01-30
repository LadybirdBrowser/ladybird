/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

class AudioNode;

// https://webaudio.github.io/web-audio-api/#AudioParam
class AudioParam final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioParam, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioParam);

public:
    enum class FixedAutomationRate : u8 {
        No,
        Yes,
    };
    static GC::Ref<AudioParam> create(JS::Realm&, GC::Ref<BaseAudioContext>, float default_value, float min_value, float max_value, Bindings::AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    virtual ~AudioParam() override;

    GC::Ref<BaseAudioContext> context() const { return m_context; }

    struct InputConnection {
        GC::Ref<AudioNode> source_node;
        WebIDL::UnsignedLong output;

        bool operator==(InputConnection const& other) const = default;
    };

    struct SetValueAtTimeEvent {
        float value { 0.0f };
        double start_time { 0.0 };

        bool operator==(SetValueAtTimeEvent const& other) const = default;
    };

    struct LinearRampToValueAtTimeEvent {
        float value { 0.0f };
        double end_time { 0.0 };

        bool operator==(LinearRampToValueAtTimeEvent const& other) const = default;
    };

    struct ExponentialRampToValueAtTimeEvent {
        float value { 0.0f };
        double end_time { 0.0 };

        bool operator==(ExponentialRampToValueAtTimeEvent const& other) const = default;
    };

    struct SetTargetAtTimeEvent {
        float target { 0.0f };
        double start_time { 0.0 };
        float time_constant { 0.0f };

        bool operator==(SetTargetAtTimeEvent const& other) const = default;
    };

    struct SetValueCurveAtTimeEvent {
        Vector<float> values;
        double start_time { 0.0 };
        double duration { 0.0 };

        bool operator==(SetValueCurveAtTimeEvent const& other) const = default;
    };

    using AutomationEvent = Variant<SetValueAtTimeEvent, LinearRampToValueAtTimeEvent, ExponentialRampToValueAtTimeEvent, SetTargetAtTimeEvent, SetValueCurveAtTimeEvent>;

    struct TimelineEvent {
        AutomationEvent event;

        bool operator==(TimelineEvent const& other) const = default;
    };

    // FIXME: The timeline/event model here is a best-effort representation of the spec automation
    // timeline. Some behaviors (notably cancelScheduledValues vs cancelAndHoldAtTime and ramp
    // interactions) may diverge from the Web Audio specification.

    ReadonlySpan<InputConnection> input_connections() const { return m_input_connections.span(); }
    ReadonlySpan<TimelineEvent> timeline_events() const { return m_timeline_events.span(); }

    float value() const;
    float unclamped_value() const { return m_current_value; }
    WebIDL::ExceptionOr<void> set_value(float);

    Bindings::AutomationRate automation_rate() const;
    WebIDL::ExceptionOr<void> set_automation_rate(Bindings::AutomationRate);

    float default_value() const;
    float min_value() const;
    float max_value() const;

    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_value_at_time(float value, double start_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> linear_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> exponential_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_target_at_time(float target, double start_time, float time_constant);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_value_curve_at_time(Span<float> values, double start_time, double duration);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> cancel_scheduled_values(double cancel_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> cancel_and_hold_at_time(double cancel_time);

    struct AutomationSegment {
        enum class Type : u8 {
            Constant,
            LinearRamp,
            ExponentialRamp,
            Target,
            ValueCurve,
        };

        Type type { Type::Constant };
        double start_time { 0.0 };
        double end_time { 0.0 }; // end_time >= start_time

        float start_value { 0.0f };
        float end_value { 0.0f };

        float time_constant { 0.0f };
        float target { 0.0f };

        Vector<float> curve;
        double curve_start_time { 0.0 };
        double curve_duration { 0.0 };
    };

    Vector<AutomationSegment> generate_automation_segments() const;

private:
    AudioParam(JS::Realm&, GC::Ref<BaseAudioContext>, float default_value, float min_value, float max_value, Bindings::AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    friend class AudioNode;

    GC::Ref<BaseAudioContext> m_context;

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-current-value-slot
    float m_current_value {}; //  [[current value]]

    float m_default_value {};

    float m_min_value {};
    float m_max_value {};

    Bindings::AutomationRate m_automation_rate {};

    FixedAutomationRate m_fixed_automation_rate { FixedAutomationRate::No };

    Vector<InputConnection> m_input_connections;

    Vector<TimelineEvent> m_timeline_events;

    static double event_sort_time(AutomationEvent const&);
    static double event_start_time(AutomationEvent const&);
    static Optional<double> event_natural_end_time(AutomationEvent const&);
    static bool is_value_curve_event(AutomationEvent const&);
    static bool value_curve_overlaps(double start_time, double end_time, SetValueCurveAtTimeEvent const&);

    bool time_overlaps_value_curve(double time, bool include_boundaries) const;

    void insert_timeline_event(AutomationEvent);
    void remove_timeline_events_after(double cancel_time);
    void update_current_value_from_timeline();
    float intrinsic_value_at_time(double) const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
