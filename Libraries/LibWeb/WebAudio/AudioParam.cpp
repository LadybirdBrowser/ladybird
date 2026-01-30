/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Bindings/AudioParamPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/DOMException.h>
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
    // [from-spec] NotSupportedError if a value curve covers the current time.
    if (time_overlaps_value_curve(m_context->current_time(), true))
        return WebIDL::NotSupportedError::create(realm(), "value setter overlaps existing value curve"_utf16);

    m_current_value = value;
    m_context->notify_audio_graph_changed();

    return {};
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
    m_context->notify_audio_graph_changed();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-defaultvalue
float AudioParam::default_value() const
{
    return m_default_value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-minvalue
float AudioParam::min_value() const
{
    return m_min_value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-maxvalue
float AudioParam::max_value() const
{
    return m_max_value;
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_at_time(float value, double start_time)
{
    // A RangeError exception MUST be thrown if startTime is negative.
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"sv };

    // [from-spec] If startTime is earlier than the context time, clamp it to currentTime for
    // retrospective automation.
    double const clamped_start_time = max(start_time, m_context->current_time());

    if (time_overlaps_value_curve(clamped_start_time, false))
        return WebIDL::NotSupportedError::create(realm(), "Event overlaps existing value curve"_utf16);

    // NOTE: Scheduled values are not clamped during automation math. Clamping happens when applying
    // the computed value to the DSP parameter.
    insert_timeline_event(SetValueAtTimeEvent { .value = value, .start_time = clamped_start_time });

    m_context->notify_audio_graph_changed();

    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-linearramptovalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::linear_ramp_to_value_at_time(float value, double end_time)
{
    // A RangeError exception MUST be thrown if endTime is negative.
    if (end_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "endTime must not be negative"sv };

    // [from-spec] If endTime is earlier than the context time, clamp it to currentTime for
    // retrospective automation.
    double const clamped_end_time = max(end_time, m_context->current_time());

    if (time_overlaps_value_curve(clamped_end_time, false))
        return WebIDL::NotSupportedError::create(realm(), "Event overlaps existing value curve"_utf16);

    insert_timeline_event(LinearRampToValueAtTimeEvent { .value = value, .end_time = clamped_end_time });
    m_context->notify_audio_graph_changed();
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-exponentialramptovalueattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::exponential_ramp_to_value_at_time(float value, double end_time)
{
    if (!isfinite(value))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Expected value to be a finite floating-point number"sv };

    // RangeError if the target is not strictly positive. Negative
    // values are accepted and handled as a degenerate exponential ramp during
    // evaluation (see generate_automation_segments).
    if (value == 0.0f)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "value must be positive for exponential ramps"sv };

    // A RangeError exception MUST be thrown if endTime is negative.
    if (end_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "endTime must not be negative"sv };

    // [from-spec] If endTime is earlier than the context time, clamp it to currentTime for
    // retrospective automation.
    double const clamped_end_time = max(end_time, m_context->current_time());

    if (time_overlaps_value_curve(clamped_end_time, false))
        return WebIDL::NotSupportedError::create(realm(), "Event overlaps existing value curve"_utf16);

    insert_timeline_event(ExponentialRampToValueAtTimeEvent { .value = value, .end_time = clamped_end_time });
    m_context->notify_audio_graph_changed();
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-settargetattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_target_at_time(float target, double start_time, float time_constant)
{
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"sv };
    if (time_constant <= 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "timeConstant must be positive"sv };

    if (time_overlaps_value_curve(start_time, false))
        return WebIDL::NotSupportedError::create(realm(), "Event overlaps existing value curve"_utf16);

    // [from-spec] If startTime is earlier than the context time, it is clamped to currentTime.
    double const clamped_start_time = max(start_time, m_context->current_time());

    insert_timeline_event(SetTargetAtTimeEvent { .target = target, .start_time = clamped_start_time, .time_constant = time_constant });
    m_context->notify_audio_graph_changed();
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::set_value_curve_at_time(Span<float> values, double start_time, double duration)
{
    if (start_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "startTime must not be negative"sv };
    if (duration <= 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "duration must be positive"sv };
    if (values.is_empty())
        return WebIDL::InvalidStateError::create(realm(), "values must not be empty"_utf16);
    if (values.size() < 2)
        return WebIDL::InvalidStateError::create(realm(), "values must contain at least two entries"_utf16);
    for (auto v : values) {
        if (!isfinite(v))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "values must be finite"sv };
    }

    // [from-spec] If startTime is earlier than the context time, clamp it to currentTime for
    // retrospective automation.
    double const clamped_start_time = max(start_time, m_context->current_time());

    double end_time = clamped_start_time + duration;

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-setvaluecurveattime
    // An exception MUST be thrown if startTime..endTime overlaps another curve or other automation.
    for (auto const& entry : m_timeline_events) {
        if (entry.event.has<SetValueCurveAtTimeEvent>()) {
            auto const& existing = entry.event.get<SetValueCurveAtTimeEvent>();
            if (value_curve_overlaps(clamped_start_time, end_time, existing))
                return WebIDL::NotSupportedError::create(realm(), "setValueCurveAtTime overlaps an existing curve"_utf16);
            continue;
        }

        // Treat other automation as occupying their keyed time point. Allow touches at the boundary.
        double existing_time_point = event_sort_time(entry.event);
        if (existing_time_point > clamped_start_time && existing_time_point < end_time)
            return WebIDL::NotSupportedError::create(realm(), "setValueCurveAtTime overlaps an existing automation"_utf16);
    }

    SetValueCurveAtTimeEvent event;
    event.start_time = clamped_start_time;
    event.duration = duration;
    event.values.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i)
        event.values[i] = values[i];

    insert_timeline_event(move(event));
    m_context->notify_audio_graph_changed();
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelscheduledvalues
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::cancel_scheduled_values(double cancel_time)
{
    if (cancel_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "cancelTime must not be negative"sv };

    // https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelscheduledvalues
    // Cancel scheduled parameter changes with times at or after cancelTime.
    // NOTE: This intentionally does not insert an implicit "hold" event. Holding the instantaneous
    // value at cancelTime is the behavior of cancelAndHoldAtTime().
    remove_timeline_events_after(cancel_time);
    m_context->notify_audio_graph_changed();
    return GC::Ref { *this };
}

// https://webaudio.github.io/web-audio-api/#dom-audioparam-cancelandholdattime
WebIDL::ExceptionOr<GC::Ref<AudioParam>> AudioParam::cancel_and_hold_at_time(double cancel_time)
{
    if (cancel_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "cancelTime must not be negative"sv };

    // Capture the instantaneous value at cancelTime against the current timeline.
    float hold_value = intrinsic_value_at_time(cancel_time);

    // Preserve or truncate events that begin strictly before cancelTime. Events that start at or
    // after the cancel point are dropped. Events that cross the cancel point are replaced with a
    // truncated equivalent that ends exactly at cancelTime so the pre-cancel timeline remains
    // intact while future automation is removed.
    Vector<TimelineEvent> preserved;
    preserved.ensure_capacity(m_timeline_events.size() + 1);

    double previous_event_time = 0.0;
    for (auto const& entry : m_timeline_events) {
        double start_time = entry.event.visit(
            [](SetValueAtTimeEvent const& e) { return e.start_time; },
            [&](LinearRampToValueAtTimeEvent const&) { return previous_event_time; },
            [&](ExponentialRampToValueAtTimeEvent const&) { return previous_event_time; },
            [](SetTargetAtTimeEvent const& e) { return e.start_time; },
            [](SetValueCurveAtTimeEvent const& e) { return e.start_time; });

        double const sort_time = event_sort_time(entry.event);
        auto const natural_end = event_natural_end_time(entry.event);
        bool const starts_after_cancel = start_time >= cancel_time;
        bool const crosses_cancel = natural_end.has_value() && start_time < cancel_time && natural_end.value() > cancel_time;

        if (starts_after_cancel) {
            previous_event_time = sort_time;
            continue;
        }

        if (crosses_cancel) {
            entry.event.visit(
                [&](LinearRampToValueAtTimeEvent const&) {
                    preserved.append(TimelineEvent { .event = LinearRampToValueAtTimeEvent { .value = hold_value, .end_time = cancel_time } });
                },
                [&](ExponentialRampToValueAtTimeEvent const&) {
                    if (hold_value > 0.0f)
                        preserved.append(TimelineEvent { .event = ExponentialRampToValueAtTimeEvent { .value = hold_value, .end_time = cancel_time } });
                    else
                        preserved.append(TimelineEvent { .event = SetValueAtTimeEvent { .value = hold_value, .start_time = cancel_time } });
                },
                [&](SetValueCurveAtTimeEvent const&) {
                    // Keep the original curve; the inserted hold event will cap evaluation at
                    // cancel_time while preserving the original duration mapping for the
                    // pre-cancel portion of the curve.
                    preserved.append(entry);
                },
                [&](auto const&) {
                    preserved.append(entry);
                });

            previous_event_time = cancel_time;
            continue;
        }

        preserved.append(entry);
        previous_event_time = sort_time;
    }

    m_timeline_events = move(preserved);

    // Insert the hold so later automation can resume from the frozen value.
    insert_timeline_event(SetValueAtTimeEvent { .value = hold_value, .start_time = cancel_time });
    m_context->notify_audio_graph_changed();
    return GC::Ref { *this };
}

double AudioParam::event_sort_time(AutomationEvent const& event)
{
    return event.visit(
        [](SetValueAtTimeEvent const& e) { return e.start_time; },
        [](LinearRampToValueAtTimeEvent const& e) { return e.end_time; },
        [](ExponentialRampToValueAtTimeEvent const& e) { return e.end_time; },
        [](SetTargetAtTimeEvent const& e) { return e.start_time; },
        [](SetValueCurveAtTimeEvent const& e) { return e.start_time; });
}

double AudioParam::event_start_time(AutomationEvent const& event)
{
    // FIXME: We do not model explicit ramp start times in the timeline and currently treat ramps as
    // starting at the previous event time during segment generation. This diverges from the spec's
    // timeline evaluation model for ramp events.
    return event.visit(
        [](SetValueAtTimeEvent const& e) { return e.start_time; },
        [](LinearRampToValueAtTimeEvent const&) { return 0.0; },
        [](ExponentialRampToValueAtTimeEvent const&) { return 0.0; },
        [](SetTargetAtTimeEvent const& e) { return e.start_time; },
        [](SetValueCurveAtTimeEvent const& e) { return e.start_time; });
}

Optional<double> AudioParam::event_natural_end_time(AutomationEvent const& event)
{
    return event.visit(
        [](SetValueAtTimeEvent const&) -> Optional<double> { return {}; },
        [](LinearRampToValueAtTimeEvent const& e) -> Optional<double> { return e.end_time; },
        [](ExponentialRampToValueAtTimeEvent const& e) -> Optional<double> { return e.end_time; },
        [](SetTargetAtTimeEvent const&) -> Optional<double> { return {}; },
        [](SetValueCurveAtTimeEvent const& e) -> Optional<double> { return e.start_time + e.duration; });
}

bool AudioParam::is_value_curve_event(AutomationEvent const& event)
{
    return event.has<SetValueCurveAtTimeEvent>();
}

bool AudioParam::value_curve_overlaps(double start_time, double end_time, SetValueCurveAtTimeEvent const& existing)
{
    double existing_start = existing.start_time;
    double existing_end = existing.start_time + existing.duration;
    return start_time < existing_end && end_time > existing_start;
}

bool AudioParam::time_overlaps_value_curve(double time, bool include_boundaries) const
{
    for (auto const& entry : m_timeline_events) {
        if (!entry.event.has<SetValueCurveAtTimeEvent>())
            continue;
        auto const& curve = entry.event.get<SetValueCurveAtTimeEvent>();
        double const curve_end = curve.start_time + curve.duration;
        if (include_boundaries) {
            if (time >= curve.start_time && time <= curve_end)
                return true;
        } else if (time > curve.start_time && time < curve_end) {
            return true;
        }
    }
    return false;
}

void AudioParam::insert_timeline_event(AutomationEvent event)
{
    double const time = event_sort_time(event);

    // Insert in time order; for equal times, preserve insertion order.
    size_t insert_index = 0;
    for (; insert_index < m_timeline_events.size(); ++insert_index) {
        auto const& existing = m_timeline_events[insert_index];
        if (event_sort_time(existing.event) > time)
            break;
    }
    m_timeline_events.insert(insert_index, TimelineEvent { .event = move(event) });
    update_current_value_from_timeline();
}

void AudioParam::remove_timeline_events_after(double cancel_time)
{
    // Remove any events that are scheduled at or after cancel_time.
    // NOTE: Ramps are keyed by their endTime in the timeline ordering.
    m_timeline_events.remove_all_matching([&](auto const& entry) {
        if (entry.event.template has<SetValueCurveAtTimeEvent>()) {
            auto const& curve = entry.event.template get<SetValueCurveAtTimeEvent>();
            // setValueCurveAtTime is keyed by startTime in ordering, but the curve affects a time
            // range. If cancel_time falls inside that range, the curve must be canceled too.
            if (curve.start_time >= cancel_time)
                return true;
            return cancel_time < (curve.start_time + curve.duration) && cancel_time > curve.start_time;
        }

        return event_sort_time(entry.event) >= cancel_time;
    });

    update_current_value_from_timeline();
}

void AudioParam::update_current_value_from_timeline()
{
    // [from-spec] The current value follows the intrinsic value at the start of each render quantum.
    m_current_value = intrinsic_value_at_time(m_context->current_time());
}

Vector<AudioParam::AutomationSegment> AudioParam::generate_automation_segments() const
{
    // Best-effort, segment-based timeline.

    Vector<AutomationSegment> segments;
    double current_time = 0.0;
    // FIXME: value() clamps to [minValue, maxValue]. Spec automation math seems to operate on
    // unclamped scheduled values, with clamping happening when applying to DSP parameters.
    float current_value = m_current_value;

    auto const next_event_time = [&](size_t index) -> Optional<double> {
        if (index + 1 >= m_timeline_events.size())
            return {};
        return event_sort_time(m_timeline_events[index + 1].event);
    };

    auto const next_event_is_ramp = [&](size_t index) -> bool {
        if (index + 1 >= m_timeline_events.size())
            return false;
        auto const& next_event = m_timeline_events[index + 1].event;
        return next_event.has<LinearRampToValueAtTimeEvent>() || next_event.has<ExponentialRampToValueAtTimeEvent>();
    };

    for (size_t i = 0; i < m_timeline_events.size(); ++i) {
        auto const& event = m_timeline_events[i].event;

        // Determine the effective start time for this segment.
        double segment_start_time = event.visit(
            [](SetValueAtTimeEvent const& e) { return e.start_time; },
            [&](LinearRampToValueAtTimeEvent const&) { return current_time; },
            [&](ExponentialRampToValueAtTimeEvent const&) { return current_time; },
            [](SetTargetAtTimeEvent const& e) { return e.start_time; },
            [](SetValueCurveAtTimeEvent const& e) { return e.start_time; });

        // FIXME: This is a simplified model for ramps: ramp start time is inferred from the previous
        // segment end. The spec's ramp timeline evaluation has more nuanced interactions.

        if (segment_start_time < 0)
            continue;

        if (segment_start_time > current_time) {
            segments.append(AutomationSegment {
                .type = AutomationSegment::Type::Constant,
                .start_time = current_time,
                .end_time = segment_start_time,
                .start_value = current_value,
                .end_value = current_value,
                .curve = {},
            });
            current_time = segment_start_time;
        }

        Optional<double> natural_end_time = event_natural_end_time(event);
        double const cap_end_time = next_event_time(i).value_or(natural_end_time.value_or(AK::NumericLimits<double>::max()));
        double segment_end_time = min(natural_end_time.value_or(cap_end_time), cap_end_time);

        if (segment_end_time < current_time)
            continue;

        event.visit(
            [&](SetValueAtTimeEvent const& e) {
                // Ramps in the WebAudio timeline are keyed by their endTime for ordering, but they
                // begin at the previous event time. If the next event is a ramp, we must not
                // extend this constant segment up to the ramp's endTime, otherwise the ramp would
                // be skipped entirely.
                if (next_event_is_ramp(i) && segment_end_time > current_time)
                    segment_end_time = current_time;

                if (segment_end_time <= current_time) {
                    current_value = e.value;
                    return;
                }

                segments.append(AutomationSegment {
                    .type = AutomationSegment::Type::Constant,
                    .start_time = current_time,
                    .end_time = segment_end_time,
                    .start_value = e.value,
                    .end_value = e.value,
                    .curve = {},
                });
                current_value = e.value;
                current_time = segment_end_time;
            },
            [&](LinearRampToValueAtTimeEvent const& e) {
                // NOTE: Spec ramp behavior is more nuanced; this is a best-effort segment.
                // FIXME: The spec ramp timeline evaluation model has nuanced interactions between
                // events. Here we cap ramps at the next event time and compute the value at the cap.
                if (segment_end_time <= current_time) {
                    // A ramp may be scheduled to end exactly at the current time (e.g. multiple
                    // ramp events at the same endTime, or a ramp following an instantaneous event
                    // at the same time). Even with zero duration, it must still apply its target
                    // value at that instant.
                    if (e.end_time <= current_time)
                        current_value = e.value;
                    return;
                }

                float end_value = e.value;
                if (segment_end_time < e.end_time) {
                    double const denom = e.end_time > current_time ? (e.end_time - current_time) : 0.0;
                    double const pos = denom > 0.0 ? clamp((segment_end_time - current_time) / denom, 0.0, 1.0) : 0.0;
                    end_value = static_cast<float>(static_cast<double>(current_value) + ((static_cast<double>(e.value) - static_cast<double>(current_value)) * pos));
                }
                segments.append(AutomationSegment {
                    .type = AutomationSegment::Type::LinearRamp,
                    .start_time = current_time,
                    .end_time = segment_end_time,
                    .start_value = current_value,
                    .end_value = end_value,
                    .curve = {},
                });
                current_value = end_value;
                current_time = segment_end_time;
            },
            [&](ExponentialRampToValueAtTimeEvent const& e) {
                // FIXME: The spec ramp timeline evaluation model has nuanced interactions between
                // events. Here we cap ramps at the next event time and compute the value at the cap.
                // [from-spec] If either v0 or v1 is less than or equal to zero, treat the event as
                // a setValueAtTime at endTime instead of attempting an exponential ramp.
                bool const invalid_exponential = current_value <= 0.0f || e.value <= 0.0f;

                if (segment_end_time <= current_time) {
                    if (e.end_time <= current_time)
                        current_value = e.value;
                    return;
                }

                if (invalid_exponential) {
                    segments.append(AutomationSegment {
                        .type = AutomationSegment::Type::Constant,
                        .start_time = current_time,
                        .end_time = segment_end_time,
                        .start_value = current_value,
                        .end_value = current_value,
                        .curve = {},
                    });

                    if (segment_end_time >= e.end_time)
                        current_value = e.value;
                    current_time = segment_end_time;
                    return;
                }

                float end_value = e.value;
                if (segment_end_time < e.end_time) {
                    double const denom = e.end_time > current_time ? (e.end_time - current_time) : 0.0;
                    double const pos = denom > 0.0 ? clamp((segment_end_time - current_time) / denom, 0.0, 1.0) : 0.0;

                    // For exponential ramps, compute the intermediate value using the same equation
                    // as intrinsic evaluation.
                    if (current_value > 0.0f && e.value > 0.0f) {
                        double const ratio = static_cast<double>(e.value) / static_cast<double>(current_value);
                        end_value = static_cast<float>(static_cast<double>(current_value) * pow(ratio, pos));
                    }
                }
                segments.append(AutomationSegment {
                    .type = AutomationSegment::Type::ExponentialRamp,
                    .start_time = current_time,
                    .end_time = segment_end_time,
                    .start_value = current_value,
                    .end_value = end_value,
                    .curve = {},
                });
                current_value = end_value;
                current_time = segment_end_time;
            },
            [&](SetTargetAtTimeEvent const& e) {
                // value(t) = target + (start-target) * exp(-(t-startTime)/timeConstant)
                AutomationSegment seg {
                    .type = AutomationSegment::Type::Target,
                    .start_time = current_time,
                    .end_time = segment_end_time,
                    .start_value = current_value,
                    .end_value = current_value,
                    .time_constant = e.time_constant,
                    .target = e.target,
                    .curve = {},
                };

                if (segment_end_time > current_time && e.time_constant > 0) {
                    double const dt = segment_end_time - current_time;
                    double const k = exp(-dt / static_cast<double>(e.time_constant));
                    seg.end_value = static_cast<float>(static_cast<double>(e.target) + ((static_cast<double>(current_value) - static_cast<double>(e.target)) * k));
                }

                segments.append(move(seg));
                current_value = segments.last().end_value;
                current_time = segment_end_time;
            },
            [&](SetValueCurveAtTimeEvent const& e) {
                AutomationSegment seg {
                    .type = AutomationSegment::Type::ValueCurve,
                    .start_time = current_time,
                    .end_time = segment_end_time,
                    .start_value = e.values.first(),
                    .end_value = e.values.last(),
                    .curve = e.values,
                    .curve_start_time = e.start_time,
                    .curve_duration = e.duration,
                };

                // If truncated early, compute the end value via linear interpolation.
                double const full_end = e.start_time + e.duration;
                if (segment_end_time < full_end && e.duration > 0 && e.values.size() >= 2) {
                    double const pos = (segment_end_time - e.start_time) / e.duration;
                    double const scaled = clamp(pos, 0.0, 1.0) * static_cast<double>(e.values.size() - 1);
                    size_t const idx = static_cast<size_t>(floor(scaled));
                    size_t const next = min(idx + 1, e.values.size() - 1);
                    double const frac = scaled - static_cast<double>(idx);
                    seg.end_value = static_cast<float>(static_cast<double>(e.values[idx]) + ((static_cast<double>(e.values[next]) - static_cast<double>(e.values[idx])) * frac));
                }

                segments.append(move(seg));
                current_value = segments.last().end_value;
                current_time = segment_end_time;
            });
    }

    // Final constant segment.
    // NOTE: If the last generated segment already extends to the sentinel "infinite" end time
    // (AK::NumericLimits<double>::max()), then current_time may be advanced to that value.
    // Emitting an additional segment starting at this sentinel is redundant and can lead to
    // overflow/inf issues when converting times to frame indices elsewhere.
    if (current_time < AK::NumericLimits<double>::max() / 2) {
        segments.append(AutomationSegment {
            .type = AutomationSegment::Type::Constant,
            .start_time = current_time,
            .end_time = AK::NumericLimits<double>::max(),
            .start_value = current_value,
            .end_value = current_value,
            .curve = {},
        });
    }

    return segments;
}

float AudioParam::intrinsic_value_at_time(double time) const
{
    if (time < 0)
        return m_current_value;

    // FIXME: This currently regenerates the segment list per query. It's correct for now, but it
    // bakes in the "best-effort" segment model and may be expensive if called frequently.
    auto segments = generate_automation_segments();
    for (auto const& seg : segments) {
        if (time < seg.start_time)
            continue;
        if (time > seg.end_time)
            continue;

        double const t0 = seg.start_time;
        double const t1 = seg.end_time;
        double const t = time;
        double const duration = max(0.0, t1 - t0);
        double const pos = duration > 0 ? clamp((t - t0) / duration, 0.0, 1.0) : 0.0;

        switch (seg.type) {
        case AutomationSegment::Type::Constant:
            return seg.start_value;
        case AutomationSegment::Type::LinearRamp:
            return static_cast<float>(static_cast<double>(seg.start_value) + ((static_cast<double>(seg.end_value) - static_cast<double>(seg.start_value)) * pos));
        case AutomationSegment::Type::ExponentialRamp: {
            // FIXME: Ensure full spec behavior for exponential ramps, including edge cases.
            // In particular, the spec defines behavior when the ramp is ill-defined due to
            // non-positive start/end values produced by prior automation.
            if (seg.start_value <= 0 || seg.end_value <= 0)
                return seg.end_value;
            double const ratio = static_cast<double>(seg.end_value) / static_cast<double>(seg.start_value);
            return static_cast<float>(static_cast<double>(seg.start_value) * pow(ratio, pos));
        }
        case AutomationSegment::Type::Target: {
            if (seg.time_constant <= 0)
                return seg.target;
            double const dt = t - t0;
            double const k = exp(-dt / static_cast<double>(seg.time_constant));
            return static_cast<float>(static_cast<double>(seg.target) + ((static_cast<double>(seg.start_value) - static_cast<double>(seg.target)) * k));
        }
        case AutomationSegment::Type::ValueCurve: {
            if (seg.curve.size() == 1)
                return seg.curve[0];
            if (seg.curve.is_empty())
                return seg.start_value;
            double const curve_duration = seg.curve_duration > 0 ? seg.curve_duration : max(0.0, seg.end_time - seg.start_time);
            double const curve_pos = curve_duration > 0 ? clamp((t - seg.curve_start_time) / curve_duration, 0.0, 1.0) : pos;
            double const scaled = curve_pos * static_cast<double>(seg.curve.size() - 1);
            size_t const idx = static_cast<size_t>(floor(scaled));
            size_t const next = min(idx + 1, seg.curve.size() - 1);
            double const frac = scaled - static_cast<double>(idx);
            return static_cast<float>(static_cast<double>(seg.curve[idx]) + ((static_cast<double>(seg.curve[next]) - static_cast<double>(seg.curve[idx])) * frac));
        }
        }
    }

    return m_current_value;
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

    for (auto const& connection : m_input_connections)
        visitor.visit(connection.source_node);
}

}
