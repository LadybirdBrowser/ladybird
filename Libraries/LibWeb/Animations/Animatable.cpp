/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Animations/Animatable.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/PseudoElementParsing.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::Animations {

struct Animatable::Transition {
    HashMap<CSS::PropertyID, size_t> transition_attribute_indices;
    Vector<TransitionAttributes> transition_attributes;
    GC::Ptr<CSS::CSSStyleDeclaration const> cached_transition_property_source;
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
    return get_animations_internal(options);
}

WebIDL::ExceptionOr<Vector<GC::Ref<Animation>>> Animatable::get_animations_internal(Optional<GetAnimationsOptions> options)
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
            relevant_animations.extend(TRY(child.get_animations(options)));
            return IterationDecision::Continue;
        }));
    }

    // The returned list is sorted using the composite order described for the associated animations of effects in
    // §5.4.2 The effect stack.
    quick_sort(relevant_animations, [](GC::Ref<Animation>& a, GC::Ref<Animation>& b) {
        auto& a_effect = as<KeyframeEffect>(*a->effect());
        auto& b_effect = as<KeyframeEffect>(*b->effect());
        return KeyframeEffect::composite_order(a_effect, b_effect) < 0;
    });

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

void Animatable::add_transitioned_properties(Optional<CSS::PseudoElement> pseudo_element, Vector<Vector<CSS::PropertyID>> properties, CSS::StyleValueVector delays, CSS::StyleValueVector durations, CSS::StyleValueVector timing_functions, CSS::StyleValueVector transition_behaviors)
{
    VERIFY(properties.size() == delays.size());
    VERIFY(properties.size() == durations.size());
    VERIFY(properties.size() == timing_functions.size());
    VERIFY(properties.size() == transition_behaviors.size());

    auto* maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;

    auto& transition = *maybe_transition;
    for (size_t i = 0; i < properties.size(); i++) {
        size_t index_of_this_transition = transition.transition_attributes.size();
        auto delay = delays[i]->is_time() ? delays[i]->as_time().time().to_milliseconds() : 0;
        auto duration = durations[i]->is_time() ? durations[i]->as_time().time().to_milliseconds() : 0;
        auto timing_function = timing_functions[i]->is_easing() ? timing_functions[i]->as_easing().function() : CSS::EasingStyleValue::CubicBezier::ease();
        auto transition_behavior = CSS::keyword_to_transition_behavior(transition_behaviors[i]->to_keyword()).value_or(CSS::TransitionBehavior::Normal);
        VERIFY(timing_functions[i]->is_easing());
        transition.transition_attributes.empend(delay, duration, timing_function, transition_behavior);

        for (auto const& property : properties[i])
            transition.transition_attribute_indices.set(property, index_of_this_transition);
    }
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

void Animatable::clear_transitions(Optional<CSS::PseudoElement> pseudo_element)
{
    auto maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;

    auto& transition = *maybe_transition;
    transition.associated_transitions.clear();
    transition.transition_attribute_indices.clear();
    transition.transition_attributes.clear();
}

void Animatable::visit_edges(JS::Cell::Visitor& visitor)
{
    auto& impl = ensure_impl();
    visitor.visit(impl.associated_animations);
    for (auto const& cached_animation_source : impl.cached_animation_name_source)
        visitor.visit(cached_animation_source);
    for (auto const& cached_animation_name : impl.cached_animation_name_animation)
        visitor.visit(cached_animation_name);
    for (auto const& transition : impl.transitions) {
        if (transition) {
            visitor.visit(transition->cached_transition_property_source);
            visitor.visit(transition->associated_transitions);
        }
    }
}

GC::Ptr<CSS::CSSStyleDeclaration const> Animatable::cached_animation_name_source(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (!m_impl)
        return {};
    auto& impl = *m_impl;
    if (pseudo_element.has_value()) {
        if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            return {};
        }
        return impl.cached_animation_name_source[to_underlying(pseudo_element.value()) + 1];
    }
    return impl.cached_animation_name_source[0];
}

void Animatable::set_cached_animation_name_source(GC::Ptr<CSS::CSSStyleDeclaration const> value, Optional<CSS::PseudoElement> pseudo_element)
{
    auto& impl = ensure_impl();
    if (pseudo_element.has_value()) {
        if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            return;
        }
        impl.cached_animation_name_source[to_underlying(pseudo_element.value()) + 1] = value;
    } else {
        impl.cached_animation_name_source[0] = value;
    }
}

GC::Ptr<Animations::Animation> Animatable::cached_animation_name_animation(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (!m_impl)
        return {};
    auto& impl = *m_impl;

    if (pseudo_element.has_value()) {
        if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            return {};
        }

        return impl.cached_animation_name_animation[to_underlying(pseudo_element.value()) + 1];
    }
    return impl.cached_animation_name_animation[0];
}

void Animatable::set_cached_animation_name_animation(GC::Ptr<Animations::Animation> value, Optional<CSS::PseudoElement> pseudo_element)
{
    auto& impl = ensure_impl();
    if (pseudo_element.has_value()) {
        if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            return;
        }

        impl.cached_animation_name_animation[to_underlying(pseudo_element.value()) + 1] = value;
    } else {
        impl.cached_animation_name_animation[0] = value;
    }
}

GC::Ptr<CSS::CSSStyleDeclaration const> Animatable::cached_transition_property_source(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto* maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return {};
    return maybe_transition->cached_transition_property_source;
}

void Animatable::set_cached_transition_property_source(Optional<CSS::PseudoElement> pseudo_element, GC::Ptr<CSS::CSSStyleDeclaration const> value)
{
    auto* maybe_transition = ensure_transition(pseudo_element);
    if (!maybe_transition)
        return;
    maybe_transition->cached_transition_property_source = value;
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
