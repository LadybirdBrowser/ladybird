/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
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

    GC::Ptr<CSS::CSSStyleDeclaration const> cached_transition_property_source() const { return m_cached_transition_property_source; }
    void set_cached_transition_property_source(GC::Ptr<CSS::CSSStyleDeclaration const> value) { m_cached_transition_property_source = value; }

    void add_transitioned_properties(Vector<Vector<CSS::PropertyID>> properties, CSS::StyleValueVector delays, CSS::StyleValueVector durations, CSS::StyleValueVector timing_functions);
    Optional<TransitionAttributes const&> property_transition_attributes(CSS::PropertyID) const;
    void set_transition(CSS::PropertyID, GC::Ref<CSS::CSSTransition>);
    void remove_transition(CSS::PropertyID);
    GC::Ptr<CSS::CSSTransition> property_transition(CSS::PropertyID) const;
    void clear_transitions();

protected:
    void visit_edges(JS::Cell::Visitor&);

private:
    Vector<GC::Ref<Animation>> m_associated_animations;
    bool m_is_sorted_by_composite_order { true };

    Array<GC::Ptr<CSS::CSSStyleDeclaration const>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> m_cached_animation_name_source;
    Array<GC::Ptr<Animations::Animation>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount) + 1> m_cached_animation_name_animation;

    HashMap<CSS::PropertyID, size_t> m_transition_attribute_indices;
    Vector<TransitionAttributes> m_transition_attributes;
    GC::Ptr<CSS::CSSStyleDeclaration const> m_cached_transition_property_source;
    HashMap<CSS::PropertyID, GC::Ref<CSS::CSSTransition>> m_associated_transitions;
};

}
