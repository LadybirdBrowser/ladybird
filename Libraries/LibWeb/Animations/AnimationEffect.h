/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Animations/RustFFI.h>
#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/AnimationEffect.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/InstalledStyle.h>
#include <LibWeb/CSS/PropertyNameAndID.h>

namespace Web::Animations {

using FillMode = Bindings::FillMode;
using PlaybackDirection = Bindings::PlaybackDirection;
using EffectTiming = Bindings::EffectTiming;
using ComputedEffectTiming = Bindings::ComputedEffectTiming;
using OptionalEffectTiming = Bindings::OptionalEffectTiming;
using EffectTimingDuration = Variant<double, Utf16String>;

Bindings::FillMode css_fill_mode_to_bindings_fill_mode(CSS::AnimationFillMode mode);
Bindings::PlaybackDirection css_animation_direction_to_bindings_playback_direction(CSS::AnimationDirection direction);
Bindings::OptionalEffectTiming to_optional_effect_timing(Bindings::EffectTiming const&);

inline RustFFI::OptionalTimeValue to_ffi(Optional<TimeValue> const& time)
{
    return { .value = time.value_or(TimeValue { TimeValue::Type::Milliseconds, 0 }), .has_value = time.has_value() };
}

inline Optional<TimeValue> from_ffi(RustFFI::OptionalTimeValue const& time)
{
    if (!time.has_value)
        return {};
    return time.value;
}

inline Optional<double> from_ffi(RustFFI::OptionalNumber const& number)
{
    if (!number.has_value)
        return {};
    return number.value;
}

// This object lives for the duration of an animation update, and is used to store per-element data about animated CSS properties.
struct AnimationUpdateContext {
    struct ElementData {
        ElementData();
        ElementData(CSS::InstalledStyle, RefPtr<CSS::ComputedStyleWorkingSet>);
        ElementData(ElementData&&);
        ElementData& operator=(ElementData&&);
        ~ElementData();

        CSS::InstalledStyle style_before_update;
        RefPtr<CSS::ComputedStyleWorkingSet> target_style;
        GC::RootVector<GC::Ref<KeyframeEffect>> effects;
        // Whether the record the update samples over was computed by this style update, so that the base
        // adjustments the sampled values feed into were made over the values the element holds now.
        bool base_is_current { false };
    };

    AnimationUpdateContext();
    // Publishes what is left unpublished.
    ~AnimationUpdateContext();

    // Samples each element's effects over the style it holds and publishes what they compose, which the destructor
    // otherwise does.
    void publish();

    // NOTE: This is lazily populated by KeyframeEffects as their respective animations are applied to an element.
    GC::RootHashMap<DOM::AbstractElement, ElementData> elements;
};

// https://www.w3.org/TR/web-animations-1/#the-animationeffect-interface
class AnimationEffect : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(AnimationEffect, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(AnimationEffect);

public:
    static Optional<CSS::EasingFunction> parse_easing_string(Utf16View value);

    Bindings::EffectTiming get_timing() const;
    Bindings::ComputedEffectTiming get_computed_timing() const;
    WebIDL::ExceptionOr<void> update_timing(Bindings::OptionalEffectTiming const& timing = {});

    TimeValue start_delay() const { return m_start_delay; }
    void set_specified_start_delay(double start_delay) { m_specified_start_delay = start_delay; }

    TimeValue end_delay() const { return m_end_delay; }
    void set_specified_end_delay(double end_delay) { m_specified_end_delay = end_delay; }

    Bindings::FillMode fill_mode() const { return m_fill_mode; }
    void set_fill_mode(Bindings::FillMode fill_mode) { m_fill_mode = fill_mode; }

    double iteration_start() const { return m_iteration_start; }
    void set_iteration_start(double iteration_start) { m_iteration_start = iteration_start; }

    double iteration_count() const { return m_iteration_count; }
    void set_iteration_count(double iteration_count) { m_iteration_count = iteration_count; }

    TimeValue const& iteration_duration() const { return m_iteration_duration; }
    void set_specified_iteration_duration(Variant<double, Utf16String> iteration_duration) { m_specified_iteration_duration = move(iteration_duration); }

    Bindings::PlaybackDirection playback_direction() const { return m_playback_direction; }
    void set_playback_direction(Bindings::PlaybackDirection playback_direction) { m_playback_direction = playback_direction; }

    CSS::EasingFunction const& timing_function() const { return m_timing_function; }
    void set_timing_function(CSS::EasingFunction value) { m_timing_function = move(value); }

    GC::Ptr<Animation> associated_animation() const { return m_associated_animation; }
    void set_associated_animation(GC::Ptr<Animation> value);

    TimeValue end_time() const;
    Optional<TimeValue> local_time() const;
    TimeValue active_duration() const;
    Optional<TimeValue> active_time() const;
    Optional<TimeValue> active_time_using_fill(Bindings::FillMode) const;

    bool is_current() const;
    bool is_in_effect() const;

    TimeValue before_active_boundary_time() const;
    TimeValue after_active_boundary_time() const;

    // Keep this enum up to date with CSSTransition::Phase.
    using Phase = RustFFI::AnimationEffectPhase;
    Phase phase() const;

    // Everything a progress calculation needs, resolved together from one local time.
    using ResolvedTiming = RustFFI::FfiResolvedEffectTiming;
    enum class EndpointInclusiveActiveInterval : bool {
        No,
        Yes,
    };
    ResolvedTiming resolve_timing() const;

    // The normalized timing the timing model reads of the effect, with `fill_mode` as its fill mode.
    RustFFI::AnimationEffectTiming effect_timing(Bindings::FillMode fill_mode) const;

    // The timing the style engine samples the effect with, wherever it runs. The effect has an associated animation.
    RustFFI::EffectTimingSnapshot timing_snapshot() const;

    Phase previous_phase() const { return m_previous_phase; }
    void set_previous_phase(Phase value) { m_previous_phase = value; }
    double previous_current_iteration() const { return m_previous_current_iteration; }
    void set_previous_current_iteration(double value) { m_previous_current_iteration = value; }

    Optional<double> current_iteration() const;
    Optional<double> current_iteration(ResolvedTiming const&) const;
    Optional<double> transformed_progress(EndpointInclusiveActiveInterval = EndpointInclusiveActiveInterval::No) const;

    void normalize_specified_timing();

    HashTable<CSS::PropertyNameAndID> const& target_properties() const { return m_target_properties; }

    virtual GC::Ptr<DOM::Element> target() const { return {}; }
    virtual bool is_keyframe_effect() const { return false; }

    virtual void update_computed_properties(AnimationUpdateContext&) = 0;

protected:
    AnimationEffect();
    virtual ~AnimationEffect() = default;

    void update_style_if_needed() const;
    void invalidate_effect();

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    GC::Ptr<AnimationTimeline> associated_timeline() const;
    Optional<TimeValue> timeline_duration() const;

    enum class WithTransformedProgress : bool {
        No,
        Yes,
    };
    ResolvedTiming resolve_timing_at(Optional<TimeValue> const& local_time, Bindings::FillMode, WithTransformedProgress = WithTransformedProgress::No, EndpointInclusiveActiveInterval = EndpointInclusiveActiveInterval::No) const;
    ResolvedTiming resolve_timing_alone() const;

    // https://drafts.csswg.org/web-animations-2/#specified-start-delay
    double m_specified_start_delay { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#start-delay
    TimeValue m_start_delay { TimeValue::Type::Milliseconds, 0.0 };

    // https://drafts.csswg.org/web-animations-2/#specified-end-delay
    double m_specified_end_delay { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#end-delay
    TimeValue m_end_delay { TimeValue::Type::Milliseconds, 0.0 };

    // https://www.w3.org/TR/web-animations-1/#fill-mode
    Bindings::FillMode m_fill_mode { Bindings::FillMode::Auto };

    // https://www.w3.org/TR/web-animations-1/#iteration-start
    double m_iteration_start { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#iteration-count
    double m_iteration_count { 1.0 };

    // https://drafts.csswg.org/web-animations-2/#specified-iteration-duration
    Variant<double, Utf16String> m_specified_iteration_duration { "auto"_utf16 };

    // https://www.w3.org/TR/web-animations-1/#iteration-duration
    // https://drafts.csswg.org/web-animations-2/#iteration-intervals
    // The initial iteration duration of an animation effect is simply its intrinsic iteration duration
    // NB: 0ms is the intrinsic iteration duration of an effect with no associated animation - we then update this value
    //     when an animation is associated for the first time.
    TimeValue m_iteration_duration = { TimeValue::Type::Milliseconds, 0.0 };

    // https://www.w3.org/TR/web-animations-1/#playback-direction
    Bindings::PlaybackDirection m_playback_direction { Bindings::PlaybackDirection::Normal };

    // https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    GC::Ptr<Animation> m_associated_animation {};

    // https://www.w3.org/TR/web-animations-1/#time-transformations
    CSS::EasingFunction m_timing_function { CSS::EasingFunction::linear() };

    mutable bool m_has_local_time_override_for_observation { false };
    mutable Optional<TimeValue> m_local_time_override_for_observation;

    // Used for calculating transitions in StyleComputer
    Phase m_previous_phase { Phase::Idle };
    double m_previous_current_iteration { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#target-property
    // Note: Only modified by child classes
    HashTable<CSS::PropertyNameAndID> m_target_properties;
};

}
