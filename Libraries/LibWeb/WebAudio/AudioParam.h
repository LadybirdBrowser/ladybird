/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioParam
class AudioParam final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioParam, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioParam);

public:
    enum class FixedAutomationRate {
        No,
        Yes,
    };
    static GC::Ref<AudioParam> create(JS::Realm&, GC::Ref<BaseAudioContext>, float default_value, float min_value, float max_value, Bindings::AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    virtual ~AudioParam() override;

    GC::Ref<BaseAudioContext> context() const { return m_context; }

    float value() const;
    WebIDL::ExceptionOr<void> set_value(float);

    // https://webaudio.github.io/web-audio-api/#computedvalue
    float intrinsic_value_at_time(double) const;

    Bindings::AutomationRate automation_rate() const;
    WebIDL::ExceptionOr<void> set_automation_rate(Bindings::AutomationRate);

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-defaultvalue
    float default_value() const { return m_default_value; }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-minvalue
    float min_value() const { return m_min_value; }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-maxvalue
    float max_value() const { return m_max_value; }

    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_value_at_time(float value, double start_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> linear_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> exponential_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_target_at_time(float target, double start_time, float time_constant);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_value_curve_at_time(Span<float> values, double start_time, double duration);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> cancel_scheduled_values(double cancel_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> cancel_and_hold_at_time(double cancel_time);

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

    using Parameterization = Variant<SetValue, LinearRamp, ExponentialRamp, SetTarget>;

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

    AudioParam(JS::Realm&, GC::Ref<BaseAudioContext>, float default_value, float min_value, float max_value, Bindings::AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    size_t first_event_index_after(double) const;

    Optional<RampStart> ramp_start_for_insertion_index(size_t) const;

    float event_value_at_time(size_t event_index, double time) const;

    // https://webaudio.github.io/web-audio-api/#computedvalue
    ParameterizationCache const& parameterization_cache_for_time(double) const;

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    WebIDL::ExceptionOr<void> insert_event(AutomationEvent);

    GC::Ref<BaseAudioContext> m_context;

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-current-value-slot
    float m_current_value {}; //  [[current value]]

    float m_default_value {};

    float m_min_value {};
    float m_max_value {};

    Bindings::AutomationRate m_automation_rate {};

    FixedAutomationRate m_fixed_automation_rate { FixedAutomationRate::No };

    // https://webaudio.github.io/web-audio-api/#dfn-automation-event
    Vector<AutomationEvent> m_automation_events;
    size_t m_next_event_id { 0 };

    mutable Optional<ParameterizationCache> m_parameterization_cache;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
