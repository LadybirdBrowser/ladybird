/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibWeb/Animations/KeyframeEffect.h>

namespace Web::CSS {

class CSSTransition;

}

namespace Web::Animations {

// https://drafts.csswg.org/web-animations-1/#dictdef-keyframeanimationoptions
struct KeyframeAnimationOptions : public KeyframeEffectOptions {
    FlyString id { ""_fly_string };
    Optional<GC::Ptr<AnimationTimeline>> timeline;
};

// https://drafts.csswg.org/web-animations-1/#dictdef-getanimationsoptions
struct GetAnimationsOptions {
    bool subtree { false };
    Optional<String> pseudo_element {};
};

// https://drafts.csswg.org/web-animations-1/#animatable
class Animatable {
public:
    struct TransitionAttributes {
        double delay;
        double duration;
        CSS::EasingStyleValue::Function timing_function;
        CSS::TransitionBehavior transition_behavior;
    };

    virtual ~Animatable() = default;

    WebIDL::ExceptionOr<GC::Ref<Animation>> animate(Optional<GC::Root<JS::Object>> keyframes, Variant<Empty, double, KeyframeAnimationOptions> options = {});
    WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> get_animations(Optional<GetAnimationsOptions> options = {});
    WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> get_animations_internal(Optional<GetAnimationsOptions> options = {});

    void associate_with_animation(GC::Ref<Animation>);
    void disassociate_with_animation(GC::Ref<Animation>);

    GC::Ptr<CSS::CSSStyleDeclaration const> cached_animation_name_source(Optional<CSS::PseudoElement>) const;
    void set_cached_animation_name_source(GC::Ptr<CSS::CSSStyleDeclaration const> value, Optional<CSS::PseudoElement>);

    GC::Ptr<Animations::Animation> cached_animation_name_animation(Optional<CSS::PseudoElement>) const;
    void set_cached_animation_name_animation(GC::Ptr<Animations::Animation> value, Optional<CSS::PseudoElement>);

    GC::Ptr<CSS::CSSStyleDeclaration const> cached_transition_property_source(Optional<CSS::PseudoElement>) const;
    void set_cached_transition_property_source(Optional<CSS::PseudoElement>, GC::Ptr<CSS::CSSStyleDeclaration const> value);

    void add_transitioned_properties(Optional<CSS::PseudoElement>, Vector<Vector<CSS::PropertyID>> properties, CSS::StyleValueVector delays, CSS::StyleValueVector durations, CSS::StyleValueVector timing_functions, CSS::StyleValueVector transition_behaviors);
    Optional<TransitionAttributes const&> property_transition_attributes(Optional<CSS::PseudoElement>, CSS::PropertyID) const;
    void set_transition(Optional<CSS::PseudoElement>, CSS::PropertyID, GC::Ref<CSS::CSSTransition>);
    void remove_transition(Optional<CSS::PseudoElement>, CSS::PropertyID);
    GC::Ptr<CSS::CSSTransition> property_transition(Optional<CSS::PseudoElement>, CSS::PropertyID) const;
    void clear_transitions(Optional<CSS::PseudoElement>);

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    struct Transition;

    struct Impl {
        Vector<GC::Ref<Animation>> associated_animations;
        bool is_sorted_by_composite_order { true };

        Array<GC::Ptr<CSS::CSSStyleDeclaration const>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> cached_animation_name_source;
        Array<GC::Ptr<Animation>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> cached_animation_name_animation;

        mutable Array<OwnPtr<Transition>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> transitions;

        ~Impl();
    };
    Impl& ensure_impl() const;
    Transition* ensure_transition(Optional<CSS::PseudoElement>) const;

    mutable OwnPtr<Impl> m_impl;
};

}
