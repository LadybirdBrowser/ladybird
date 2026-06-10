/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::WebAudio {

using AutomationRate = Bindings::AutomationRate;

// https://webaudio.github.io/web-audio-api/#AudioParam
class AudioParam final : public Bindings::Wrappable {
    WEB_WRAPPABLE(AudioParam, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(AudioParam);

public:
    enum class FixedAutomationRate {
        No,
        Yes,
    };
    static GC::Ref<AudioParam> create(GC::Ref<BaseAudioContext>, float default_value, float min_value, float max_value, AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    virtual ~AudioParam() override;

    GC::Ref<BaseAudioContext> context() const { return m_context; }

    float value() const;
    void set_value(float);

    AutomationRate automation_rate() const;
    WebIDL::ExceptionOr<void> set_automation_rate(AutomationRate);

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

private:
    AudioParam(GC::Ref<BaseAudioContext>, float default_value, float min_value, float max_value, AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    GC::Ref<BaseAudioContext> m_context;

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-current-value-slot
    float m_current_value {}; //  [[current value]]

    float m_default_value {};

    float m_min_value {};
    float m_max_value {};

    AutomationRate m_automation_rate {};

    FixedAutomationRate m_fixed_automation_rate { FixedAutomationRate::No };

    virtual void visit_edges(GC::Cell::Visitor&) override;
};

}
