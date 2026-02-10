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
#include <LibWeb/Export.h>

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
class WEB_API Animatable {
public:
    struct TransitionAttributes {
        double delay;
        double duration;
        CSS::EasingFunction timing_function;
        CSS::TransitionBehavior transition_behavior;
    };

    virtual ~Animatable() = default;

    enum class GetAnimationsSorted {
        No,
        Yes
    };

    WebIDL::ExceptionOr<GC::Ref<Animation>> animate(Optional<GC::Root<JS::Object>> keyframes, Variant<Empty, double, KeyframeAnimationOptions> options = {});
    WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> get_animations(Optional<GetAnimationsOptions> options = {});
    WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> get_animations_internal(GetAnimationsSorted sorted, Optional<GetAnimationsOptions> options = {});

    void associate_with_animation(GC::Ref<Animation>);
    void disassociate_with_animation(GC::Ref<Animation>);

    void set_has_css_defined_animations();
    bool has_css_defined_animations() const;
    HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>* css_defined_animations(Optional<CSS::PseudoElement>);
    void add_css_animation(FlyString name, Optional<CSS::PseudoElement>, GC::Ref<CSS::CSSAnimation>);
    void remove_css_animation(FlyString name, Optional<CSS::PseudoElement>);

    void add_transitioned_properties(Optional<CSS::PseudoElement>, Vector<CSS::TransitionProperties> const& transitions);
    Vector<CSS::PropertyID> property_ids_with_matching_transition_property_entry(Optional<CSS::PseudoElement>) const;
    Optional<TransitionAttributes const&> property_transition_attributes(Optional<CSS::PseudoElement>, CSS::PropertyID) const;
    void set_transition(Optional<CSS::PseudoElement>, CSS::PropertyID, GC::Ref<CSS::CSSTransition>);
    void remove_transition(Optional<CSS::PseudoElement>, CSS::PropertyID);
    Vector<CSS::PropertyID> property_ids_with_existing_transitions(Optional<CSS::PseudoElement>) const;
    GC::Ptr<CSS::CSSTransition> property_transition(Optional<CSS::PseudoElement>, CSS::PropertyID) const;
    void clear_registered_transitions(Optional<CSS::PseudoElement>);

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    struct Transition;

    struct Impl {
        Vector<GC::Ref<Animation>> associated_animations;
        bool is_sorted_by_composite_order { true };
        bool has_css_defined_animations { false };

        mutable Array<OwnPtr<HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> css_defined_animations;
        mutable Array<OwnPtr<Transition>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> transitions;

        ~Impl();

        void visit_edges(JS::Cell::Visitor&);
    };
    Impl& ensure_impl() const;
    Transition* ensure_transition(Optional<CSS::PseudoElement>) const;

    mutable OwnPtr<Impl> m_impl;
};

}
