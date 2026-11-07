/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/AudioParamTimeline.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio {

AudioParamTimeline::AudioParamTimeline(float default_value)
    : m_timeline(RustFFI::web_audio_param_timeline_create(default_value))
{
    static_assert(to_underlying(InsertResult::Success) == to_underlying(RustFFI::InsertResult::Success));
    static_assert(to_underlying(InsertResult::EventContainedInValueCurve) == to_underlying(RustFFI::InsertResult::EventContainedInValueCurve));
    static_assert(to_underlying(InsertResult::ValueCurveOverlapsEvent) == to_underlying(RustFFI::InsertResult::ValueCurveOverlapsEvent));
}

AudioParamTimeline::~AudioParamTimeline()
{
    RustFFI::web_audio_param_timeline_destroy(m_timeline);
}

float AudioParamTimeline::value_at_time(double time) const
{
    return RustFFI::web_audio_param_timeline_value_at_time(m_timeline, time);
}

void AudioParamTimeline::sample(double start_time, double sample_period, Span<float> output) const
{
    RustFFI::web_audio_param_timeline_sample(m_timeline, start_time, sample_period, output);
}

bool AudioParamTimeline::has_events() const
{
    return RustFFI::web_audio_param_timeline_has_events(m_timeline);
}

float AudioParamTimeline::default_value() const
{
    return RustFFI::web_audio_param_timeline_default_value(m_timeline);
}

AudioParamTimeline::InsertResult AudioParamTimeline::set_value_at_time(float value, double start_time)
{
    return static_cast<InsertResult>(RustFFI::web_audio_param_timeline_set_value_at_time(m_timeline, value, start_time));
}

AudioParamTimeline::InsertResult AudioParamTimeline::linear_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value)
{
    return static_cast<InsertResult>(RustFFI::web_audio_param_timeline_linear_ramp_to_value_at_time(m_timeline, value, end_time, current_time, current_value));
}

AudioParamTimeline::InsertResult AudioParamTimeline::exponential_ramp_to_value_at_time(float value, double end_time, double current_time, float current_value)
{
    return static_cast<InsertResult>(RustFFI::web_audio_param_timeline_exponential_ramp_to_value_at_time(m_timeline, value, end_time, current_time, current_value));
}

AudioParamTimeline::InsertResult AudioParamTimeline::set_target_at_time(float target, double start_time, float time_constant)
{
    return static_cast<InsertResult>(RustFFI::web_audio_param_timeline_set_target_at_time(m_timeline, target, start_time, time_constant));
}

AudioParamTimeline::InsertResult AudioParamTimeline::set_value_curve_at_time(Vector<float> values, double start_time, double duration)
{
    return static_cast<InsertResult>(RustFFI::web_audio_param_timeline_set_value_curve_at_time(m_timeline, values.span(), start_time, duration));
}

void AudioParamTimeline::cancel_scheduled_values(double cancel_time)
{
    RustFFI::web_audio_param_timeline_cancel_scheduled_values(m_timeline, cancel_time);
}

void AudioParamTimeline::cancel_and_hold_at_time(double cancel_time)
{
    RustFFI::web_audio_param_timeline_cancel_and_hold_at_time(m_timeline, cancel_time);
}

}
