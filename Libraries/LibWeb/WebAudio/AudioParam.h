/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioParam.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebAudio/AudioParamTimeline.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio {

using AutomationRate = Bindings::AutomationRate;

// https://webaudio.github.io/web-audio-api/#AudioParam
class AudioParam final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(AudioParam, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(AudioParam);

public:
    enum class FixedAutomationRate {
        No,
        Yes,
    };
    // The owner is the AudioNode this parameter belongs to, or null for AudioListener parameters.
    static GC::Ref<AudioParam> create(GC::Ref<BaseAudioContext>, GC::Ptr<AudioNode> owner, float default_value, float min_value, float max_value, AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    virtual ~AudioParam() override;

    GC::Ref<BaseAudioContext> context() const { return m_context; }

    float value() const;
    WebIDL::ExceptionOr<void> set_value(float);

    // https://webaudio.github.io/web-audio-api/#computedvalue
    float intrinsic_value_at_time(double time) const { return m_timeline->value_at_time(time); }

    AutomationRate automation_rate() const;
    WebIDL::ExceptionOr<void> set_automation_rate(AutomationRate);

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-defaultvalue
    float default_value() const { return m_default_value; }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-minvalue
    float min_value() const { return m_min_value; }

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-maxvalue
    float max_value() const { return m_max_value; }

    NonnullRefPtr<AudioParamTimeline> timeline() const { return m_timeline; }
    NonnullRefPtr<Rendering::RenderAudioParam> render_param() const { return m_render_param; }

    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_value_at_time(float value, double start_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> linear_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> exponential_ramp_to_value_at_time(float value, double end_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_target_at_time(float target, double start_time, float time_constant);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> set_value_curve_at_time(Span<float> values, double start_time, double duration);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> cancel_scheduled_values(double cancel_time);
    WebIDL::ExceptionOr<GC::Ref<AudioParam>> cancel_and_hold_at_time(double cancel_time);

private:
    WebIDL::ExceptionOr<void> map_insert_result(AudioParamTimeline::InsertResult);

    AudioParam(GC::Ref<BaseAudioContext>, GC::Ptr<AudioNode> owner, float default_value, float min_value, float max_value, AutomationRate, FixedAutomationRate = FixedAutomationRate::No);

    GC::Ref<BaseAudioContext> m_context;

    // An AudioParam is a facet of the AudioNode that exposes it, so holding on to the parameter keeps that node, and
    // with it the node's render node and automation, alive.
    GC::Ptr<AudioNode> m_owner;

    NonnullRefPtr<AudioParamTimeline> m_timeline;
    NonnullRefPtr<Rendering::RenderAudioParam> m_render_param;

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
