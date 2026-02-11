/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/Bindings/CSSAnimationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSAnimation);

GC::Ref<CSSAnimation> CSSAnimation::create(JS::Realm& realm)
{
    return realm.create<CSSAnimation>(realm);
}

// https://www.w3.org/TR/css-animations-2/#animation-composite-order
int CSSAnimation::class_specific_composite_order(GC::Ref<Animations::Animation> other_animation) const
{
    auto other = GC::Ref { as<CSSAnimation>(*other_animation) };

    // The existence of an owning element determines the animation class, so both animations should have their owning
    // element in the same state
    VERIFY(owning_element().has_value() == other->owning_element().has_value());

    // Within the set of CSS Animations with an owning element, two animations A and B are sorted in composite order
    // (first to last) as follows:
    if (owning_element().has_value()) {
        // 1. If the owning element of A and B differs, sort A and B by tree order of their corresponding owning elements.
        //    With regard to pseudo-elements, the sort order is as follows:
        //    - element
        //    - ::marker
        //    - ::before
        //    - any other pseudo-elements not mentioned specifically in this list, sorted in ascending order by the Unicode
        //      codepoints that make up each selector
        //    - ::after
        //    - element children
        if (owning_element() != other->owning_element()) {
            // FIXME: Sort by tree order
            return 0;
        }

        // 2. Otherwise, sort A and B based on their position in the computed value of the animation-name property of the
        //    (common) owning element.
        // FIXME: Do this when animation-name supports multiple values
        return 0;
    }

    // The composite order of CSS Animations without an owning element is based on their position in the global animation list.
    return global_animation_list_order() - other->global_animation_list_order();
}

Animations::AnimationClass CSSAnimation::animation_class() const
{
    if (owning_element().has_value())
        return Animations::AnimationClass::CSSAnimationWithOwningElement;
    return Animations::AnimationClass::CSSAnimationWithoutOwningElement;
}

// NB: Unrelated style changes shouldn't cause us to recreate anonymous timelines, to achieve this we drop updates
//     between two equivalent anonymous timelines.
static bool should_update_timeline(GC::Ptr<Animations::AnimationTimeline> old_timeline, GC::Ptr<Animations::AnimationTimeline> new_timeline)
{
    if (!old_timeline || !new_timeline)
        return true;

    if (is<Animations::ScrollTimeline>(*old_timeline) && is<Animations::ScrollTimeline>(*new_timeline)) {
        auto const& old_scroll_timeline = as<Animations::ScrollTimeline>(*old_timeline);
        auto const& new_scroll_timeline = as<Animations::ScrollTimeline>(*new_timeline);

        if (!old_scroll_timeline.source_internal().has<Animations::ScrollTimeline::AnonymousSource>() || !new_scroll_timeline.source_internal().has<Animations::ScrollTimeline::AnonymousSource>())
            return true;

        return old_scroll_timeline.source_internal().get<Animations::ScrollTimeline::AnonymousSource>() != new_scroll_timeline.source_internal().get<Animations::ScrollTimeline::AnonymousSource>();
    }

    return true;
}

void CSSAnimation::apply_css_properties(ComputedProperties::AnimationProperties const& animation_properties)
{
    // FIXME: Don't apply overriden properties as defined here: https://drafts.csswg.org/css-animations-2/#animations

    VERIFY(effect());

    auto& effect = as<Animations::KeyframeEffect>(*this->effect());

    if (!m_ignored_css_properties.contains(PropertyID::AnimationTimeline) && should_update_timeline(timeline(), animation_properties.timeline)) {
        HTML::TemporaryExecutionContext context(realm());
        set_timeline(animation_properties.timeline);
    }

    effect.set_specified_iteration_duration(animation_properties.duration);
    effect.set_specified_start_delay(animation_properties.delay);
    effect.set_iteration_count(animation_properties.iteration_count);
    // https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
    // Timing properties may also be updated due to a style change. Any change to a CSS animation property that affects
    // timing requires rerunning the procedure to normalize specified timing.
    effect.normalize_specified_timing();
    effect.set_timing_function(animation_properties.timing_function);
    effect.set_fill_mode(Animations::css_fill_mode_to_bindings_fill_mode(animation_properties.fill_mode));
    effect.set_playback_direction(Animations::css_animation_direction_to_bindings_playback_direction(animation_properties.direction));
    effect.set_composite(Animations::css_animation_composition_to_bindings_composite_operation(animation_properties.composition));

    if (animation_properties.play_state != last_css_animation_play_state()) {
        if (animation_properties.play_state == CSS::AnimationPlayState::Running && play_state() != Bindings::AnimationPlayState::Running) {
            HTML::TemporaryExecutionContext context(realm());
            play().release_value_but_fixme_should_propagate_errors();
        } else if (animation_properties.play_state == CSS::AnimationPlayState::Paused && play_state() != Bindings::AnimationPlayState::Paused) {
            HTML::TemporaryExecutionContext context(realm());
            pause().release_value_but_fixme_should_propagate_errors();
        }

        set_last_css_animation_play_state(animation_properties.play_state);
    }
}

void CSSAnimation::set_timeline_for_bindings(GC::Ptr<Animations::AnimationTimeline> timeline)
{
    // AD-HOC: When the timeline of a CSS animation is modified by the author from JS we should no longer apply changes
    //         to the `animation-timeline` property. See https://github.com/w3c/csswg-drafts/issues/13472
    m_ignored_css_properties.set(PropertyID::AnimationTimeline);
    set_timeline(timeline);
}

CSSAnimation::CSSAnimation(JS::Realm& realm)
    : Animations::Animation(realm)
{
    // FIXME:
    // CSS Animations generated using the markup defined in this specification are not added to the global animation
    // list when they are created. Instead, these animations are appended to the global animation list at the first
    // moment when they transition out of the idle play state after being disassociated from their owning element. CSS
    // Animations that have been disassociated from their owning element but are still idle do not have a defined
    // composite order.
}

void CSSAnimation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSAnimation);
    Base::initialize(realm);
}

}
