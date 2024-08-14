/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/AnimationEffectPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>

namespace Web::Animations {

// // https://www.w3.org/TR/web-animations-1/#the-effecttiming-dictionaries
struct OptionalEffectTiming {
    Optional<double> delay {};
    Optional<double> end_delay {};
    Optional<Bindings::FillMode> fill {};
    Optional<double> iteration_start {};
    Optional<double> iterations {};
    Optional<Variant<double, String>> duration;
    Optional<Bindings::PlaybackDirection> direction {};
    Optional<String> easing {};
};

// // https://www.w3.org/TR/web-animations-1/#the-effecttiming-dictionaries
struct EffectTiming {
    double delay { 0 };
    double end_delay { 0 };
    Bindings::FillMode fill { Bindings::FillMode::Auto };
    double iteration_start { 0.0 };
    double iterations { 1.0 };
    Variant<double, String> duration { "auto"_string };
    Bindings::PlaybackDirection direction { Bindings::PlaybackDirection::Normal };
    String easing { "linear"_string };

    OptionalEffectTiming to_optional_effect_timing() const;
};

// https://www.w3.org/TR/web-animations-1/#the-computedeffecttiming-dictionary
struct ComputedEffectTiming : public EffectTiming {
    double end_time;
    double active_duration;
    Optional<double> local_time;
    Optional<double> progress;
    Optional<double> current_iteration;
};

enum class AnimationDirection {
    Forwards,
    Backwards,
};

Bindings::FillMode css_fill_mode_to_bindings_fill_mode(CSS::AnimationFillMode mode);
Bindings::PlaybackDirection css_animation_direction_to_bindings_playback_direction(CSS::AnimationDirection direction);

// https://www.w3.org/TR/web-animations-1/#the-animationeffect-interface
class AnimationEffect : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AnimationEffect, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(AnimationEffect);

public:
    static RefPtr<CSS::CSSStyleValue const> parse_easing_string(JS::Realm& realm, StringView value);

    EffectTiming get_timing() const;
    ComputedEffectTiming get_computed_timing() const;
    WebIDL::ExceptionOr<void> update_timing(OptionalEffectTiming timing = {});

    double start_delay() const { return m_start_delay; }
    void set_start_delay(double start_delay) { m_start_delay = start_delay; }

    double end_delay() const { return m_end_delay; }
    void set_end_delay(double end_delay) { m_end_delay = end_delay; }

    Bindings::FillMode fill_mode() const { return m_fill_mode; }
    void set_fill_mode(Bindings::FillMode fill_mode) { m_fill_mode = fill_mode; }

    double iteration_start() const { return m_iteration_start; }
    void set_iteration_start(double iteration_start) { m_iteration_start = iteration_start; }

    double iteration_count() const { return m_iteration_count; }
    void set_iteration_count(double iteration_count) { m_iteration_count = iteration_count; }

    Variant<double, String> const& iteration_duration() const { return m_iteration_duration; }
    void set_iteration_duration(Variant<double, String> iteration_duration) { m_iteration_duration = move(iteration_duration); }

    Bindings::PlaybackDirection playback_direction() const { return m_playback_direction; }
    void set_playback_direction(Bindings::PlaybackDirection playback_direction) { m_playback_direction = playback_direction; }

    CSS::EasingStyleValue::Function const& timing_function() { return m_timing_function; }
    void set_timing_function(CSS::EasingStyleValue::Function value) { m_timing_function = move(value); }

    JS::GCPtr<Animation> associated_animation() const { return m_associated_animation; }
    void set_associated_animation(JS::GCPtr<Animation> value);

    AnimationDirection animation_direction() const;

    double end_time() const;
    Optional<double> local_time() const;
    double active_duration() const;
    Optional<double> active_time() const;
    Optional<double> active_time_using_fill(Bindings::FillMode) const;

    bool is_in_play() const;
    bool is_current() const;
    bool is_in_effect() const;

    double before_active_boundary_time() const;
    double after_active_boundary_time() const;

    bool is_in_the_before_phase() const;
    bool is_in_the_after_phase() const;
    bool is_in_the_active_phase() const;
    bool is_in_the_idle_phase() const;

    enum class Phase {
        Before,
        Active,
        After,
        Idle,
    };
    Phase phase() const;

    Phase previous_phase() const { return m_previous_phase; }
    void set_previous_phase(Phase value) { m_previous_phase = value; }
    double previous_current_iteration() const { return m_previous_current_iteration; }
    void set_previous_current_iteration(double value) { m_previous_current_iteration = value; }

    Optional<double> overall_progress() const;
    Optional<double> directed_progress() const;
    AnimationDirection current_direction() const;
    Optional<double> simple_iteration_progress() const;
    Optional<double> current_iteration() const;
    Optional<double> transformed_progress() const;

    HashTable<CSS::PropertyID> const& target_properties() const { return m_target_properties; }

    virtual DOM::Element* target() const { return {}; }
    virtual bool is_keyframe_effect() const { return false; }

    virtual void update_style_properties() = 0;

protected:
    AnimationEffect(JS::Realm&);
    virtual ~AnimationEffect() = default;

    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;

    // https://www.w3.org/TR/web-animations-1/#start-delay
    double m_start_delay { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#end-delay
    double m_end_delay { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#fill-mode
    Bindings::FillMode m_fill_mode { Bindings::FillMode::Auto };

    // https://www.w3.org/TR/web-animations-1/#iteration-start
    double m_iteration_start { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#iteration-count
    double m_iteration_count { 1.0 };

    // https://www.w3.org/TR/web-animations-1/#iteration-duration
    Variant<double, String> m_iteration_duration { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#playback-direction
    Bindings::PlaybackDirection m_playback_direction { Bindings::PlaybackDirection::Normal };

    // https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    JS::GCPtr<Animation> m_associated_animation {};

    // https://www.w3.org/TR/web-animations-1/#time-transformations
    CSS::EasingStyleValue::Function m_timing_function { CSS::EasingStyleValue::Linear {} };

    // Used for calculating transitions in StyleComputer
    Phase m_previous_phase { Phase::Idle };
    double m_previous_current_iteration { 0.0 };

    // https://www.w3.org/TR/web-animations-1/#target-property
    // Note: Only modified by child classes
    HashTable<CSS::PropertyID> m_target_properties;
};

}
