/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioParam
class AudioParam final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioParam, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(AudioParam);

public:
    static JS::NonnullGCPtr<AudioParam> create(JS::Realm&, float default_value, float min_value, float max_value, Bindings::AutomationRate);

    virtual ~AudioParam() override;

    float value() const;
    void set_value(float);

    Bindings::AutomationRate automation_rate() const;
    WebIDL::ExceptionOr<void> set_automation_rate(Bindings::AutomationRate);

    float default_value() const;
    float min_value() const;
    float max_value() const;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> set_value_at_time(float value, double start_time);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> linear_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> exponential_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> set_target_at_time(float target, double start_time, float time_constant);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> set_value_curve_at_time(Span<float> values, double start_time, double duration);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> cancel_scheduled_values(double cancel_time);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioParam>> cancel_and_hold_at_time(double cancel_time);

private:
    AudioParam(JS::Realm&, float default_value, float min_value, float max_value, Bindings::AutomationRate);

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-current-value-slot
    float m_current_value {}; //  [[current value]]

    float m_default_value {};

    float m_min_value {};
    float m_max_value {};

    Bindings::AutomationRate m_automation_rate {};

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
