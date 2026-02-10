/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Animatable.h"
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/PseudoElementParsing.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::Animations {

struct Animatable::Transition {
    HashMap<CSS::PropertyID, size_t> transition_attribute_indices;
    Vector<TransitionAttributes> transition_attributes;
    HashMap<CSS::PropertyID, GC::Ref<CSS::CSSTransition>> associated_transitions;
};

Animatable::Impl::~Impl() = default;

// https://www.w3.org/TR/web-animations-1/#dom-animatable-animate
WebIDL::ExceptionOr<GC::Ref<Animation>> Animatable::animate(Optional<GC::Root<JS::Object>> keyframes, Variant<Empty, double, KeyframeAnimationOptions> options)
{
    // 1. Let target be the object on which this method was called.
    GC::Ref target { *static_cast<DOM::Element*>(this) };
    auto& realm = target->realm();

    // 2. Construct a new KeyframeEffect object, effect, in the relevant Realm of target by using the same procedure as
    //    the KeyframeEffect(target, keyframes, options) constructor, passing target as the target argument, and the
    //    keyframes and options arguments as supplied.
    //
    //    If the above procedure causes an exception to be thrown, propagate the exception and abort this procedure.
    auto effect = TRY(options.visit(
        [&](Empty) { return KeyframeEffect::construct_impl(realm, target, keyframes); },
        [&](auto const& value) { return KeyframeEffect::construct_impl(realm, target, keyframes, value); }));

    // 3. If options is a KeyframeAnimationOptions object, let timeline be the timeline member of options or, if
    //    timeline member of options is missing, be the default document timeline of the node document of the element
    //    on which this method was called.
    Optional<GC::Ptr<AnimationTimeline>> timeline;
    if (options.has<KeyframeAnimationOptions>())
        timeline = options.get<KeyframeAnimationOptions>().timeline;
    if (!timeline.has_value())
        timeline = target->document().timeline();

    // 4. Construct a new Animation object, animation, in the relevant Realm of target by using the same procedure as
    //    the Animation() constructor, passing effect and timeline as arguments of the same name.
    auto animation = TRY(Animation::construct_impl(realm, effect, move(timeline)));

    // 5. If options is a KeyframeAnimationOptions object, assign the value of the id member of options to animation’s
    //    id attribute.
    if (options.has<KeyframeAnimationOptions>())
        animation->set_id(options.get<KeyframeAnimationOptions>().id);

    //  6. Run the procedure to play an animation for animation with the auto-rewind flag set to true.
    TRY(animation->play_an_animation(Animation::AutoRewind::Yes));

    // 7. Return animation.
    return animation;
}

// https://drafts.csswg.org/web-animations-1/#dom-animatable-getanimations
WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> Animatable::get_animations(Optional<GetAnimationsOptions> options)
{
    as<DOM::Element>(*this).document().update_style();
    return get_animations_internal(GetAnimationsSorted::Yes, options);
}

WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> Animatable::get_animations_internal(GetAnimationsSorted sorted, Optional<GetAnimationsOptions> options)
{
    // 1. Let object be the object on which this method was called.

    // 2. Let pseudoElement be the result of pseudo-element parsing applied to pseudoElement of options, or null if options is not passed.
    // FIXME: Currently only DOM::Element includes Animatable, but that might not always be true.
    Optional<CSS::Selector::PseudoElementSelector> pseudo_element;
    if (options.has_value() && options->pseudo_element.has_value()) {
        auto& realm = static_cast<DOM::Element&>(*this).realm();
        pseudo_element = TRY(pseudo_element_parsing(realm, options->pseudo_element));
    }

    // 3. If pseudoElement is not null, then let target be the pseudo-element identified by pseudoElement with object as the originating element.
    //    Otherwise, let target be object.
    // FIXME: We can't refer to pseudo-elements directly, and they also can't be animated yet.
    (void)pseudo_element;
    GC::Ref target { *static_cast<DOM::Element*>(this) };

    // 4. If options is passed with subtree set to true, then return the set of relevant animations for a subtree of target.
    //    Otherwise, return the set of relevant animations for target.
    Vector<GC::Ref<Animation>> relevant_animations;
    if (m_impl) {
        auto& associated_animations = m_impl->associated_animations;
        for (auto const& animation : associated_animations) {
            if (animation->is_relevant())
                relevant_animations.append(*animation);
        }
    }

    if (options.has_value() && options->subtree) {
        Optional<WebIDL::Exception> exception;
        TRY(target->for_each_child_of_type_fallible<DOM::Element>([&](auto& child) -> WebIDL::ExceptionOr<IterationDecision> {
            relevant_animations.extend(TRY(child.get_animations_internal(GetAnimationsSorted::No, options)));
            return IterationDecision::Continue;
        }));
    }

    // The returned list is sorted using the composite order described for the associated animations of effects in
    // §5.4.2 The effect stack.
    if (sorted == GetAnimationsSorted::Yes) {
        quick_sort(relevant_animations, [](GC::Ref<Animation>& a, GC::Ref<Animation>& b) {
            auto& a_effect = as<KeyframeEffect>(*a->effect());
            auto& b_effect = as<KeyframeEffect>(*b->effect());
            return KeyframeEffect::composite_order(a_effect, b_effect) < 0;
        });
    }

    return relevant_animations;
}

void Animatable::associate_with_animation(GC::Ref<Animation> animation)
{
    auto& impl = ensure_impl();
    impl.associated_animations.append(animation);
    impl.is_sorted_by_composite_order = false;
}

void Animatable::disassociate_with_animation(GC::Ref<Animation> animation)
{
    auto& impl = *m_impl;
    impl.associated_animations.remove_first_matching([&](auto element) { return animation == element; });
}

void Animatable::add_transitioned_properties(Optional<CSS::PseudoElement> pseudo_element, Vector<CSS::TransitionProperties> const& transitions)
{
    auto* maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;

    auto& transition = *maybe_transition;
    for (size_t i = 0; i < transitions.size(); i++) {
        size_t index_of_this_transition = transition.transition_attributes.size();
        transition.transition_attributes.empend(transitions[i].delay, transitions[i].duration, transitions[i].timing_function, transitions[i].transition_behavior);

        for (auto const& property : transitions[i].properties)
            transition.transition_attribute_indices.set(property, index_of_this_transition);
    }
}

Vector<CSS::PropertyID> Animatable::property_ids_with_matching_transition_property_entry(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto const* maybe_transition = ensure_transition(pseudo_element);

    if (!maybe_transition)
        return {};

    return maybe_transition->transition_attribute_indices.keys();
}

Optional<Animatable::TransitionAttributes const&> Animatable::property_transition_attributes(Optional<CSS::PseudoElement> pseudo_element, CSS::PropertyID property) const
{
    auto* maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return {};
    auto& transition = *maybe_transition;
    if (auto maybe_attr_index = transition.transition_attribute_indices.get(property); maybe_attr_index.has_value())
        return transition.transition_attributes[maybe_attr_index.value()];
    return {};
}

Vector<CSS::PropertyID> Animatable::property_ids_with_existing_transitions(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto const* maybe_transition = ensure_transition(pseudo_element);

    if (!maybe_transition)
        return {};

    return maybe_transition->associated_transitions.keys();
}

GC::Ptr<CSS::CSSTransition> Animatable::property_transition(Optional<CSS::PseudoElement> pseudo_element, CSS::PropertyID property) const
{
    auto* maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return {};
    auto& transition = *maybe_transition;
    if (auto maybe_animation = transition.associated_transitions.get(property); maybe_animation.has_value())
        return maybe_animation.value();
    return {};
}

void Animatable::set_transition(Optional<CSS::PseudoElement> pseudo_element, CSS::PropertyID property, GC::Ref<CSS::CSSTransition> animation)
{
    auto maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;
    auto& transition = *maybe_transition;
    VERIFY(!transition.associated_transitions.contains(property));
    transition.associated_transitions.set(property, animation);
}

void Animatable::remove_transition(Optional<CSS::PseudoElement> pseudo_element, CSS::PropertyID property_id)
{
    auto maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;
    auto& transition = *maybe_transition;
    VERIFY(transition.associated_transitions.contains(property_id));
    transition.associated_transitions.remove(property_id);
}

void Animatable::clear_registered_transitions(Optional<CSS::PseudoElement> pseudo_element)
{
    auto maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;

    auto& transition = *maybe_transition;
    transition.transition_attribute_indices.clear();
    transition.transition_attributes.clear();
}

void Animatable::visit_edges(JS::Cell::Visitor& visitor)
{
    if (m_impl)
        m_impl->visit_edges(visitor);
}

void Animatable::Impl::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(associated_animations);
    for (auto const& css_animation : css_defined_animations) {
        if (css_animation)
            visitor.visit(*css_animation);
    }

    for (auto const& transition : transitions) {
        if (transition)
            visitor.visit(transition->associated_transitions);
    }
}

void Animatable::set_has_css_defined_animations()
{
    ensure_impl().has_css_defined_animations = true;
}

bool Animatable::has_css_defined_animations() const
{
    if (!m_impl)
        return false;

    return m_impl->has_css_defined_animations;
}

HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>* Animatable::css_defined_animations(Optional<CSS::PseudoElement> pseudo_element)
{
    auto& impl = ensure_impl();

    if (pseudo_element.has_value() && !CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value()))
        return nullptr;

    auto index = pseudo_element
                     .map([](CSS::PseudoElement pseudo_element_value) { return to_underlying(pseudo_element_value) + 1; })
                     .value_or(0);

    if (!impl.css_defined_animations[index])
        impl.css_defined_animations[index] = make<HashMap<FlyString, GC::Ref<CSS::CSSAnimation>>>();

    return impl.css_defined_animations[index];
}

Animatable::Impl& Animatable::ensure_impl() const
{
    if (!m_impl)
        m_impl = make<Impl>();
    return *m_impl;
}

Animatable::Transition* Animatable::ensure_transition(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto& impl = ensure_impl();

    size_t pseudo_element_index = 0;
    if (pseudo_element.has_value()) {
        if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value()))
            return nullptr;
        pseudo_element_index = to_underlying(pseudo_element.value()) + 1;
    }

    if (!impl.transitions[pseudo_element_index])
        impl.transitions[pseudo_element_index] = make<Transition>();
    return impl.transitions[pseudo_element_index];
}

}
