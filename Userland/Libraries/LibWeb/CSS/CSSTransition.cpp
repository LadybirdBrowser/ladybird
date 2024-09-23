/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/CSSTransitionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSTransition);

JS::NonnullGCPtr<CSSTransition> CSSTransition::start_a_transition(DOM::Element& element, PropertyID property_id, size_t transition_generation,
    double start_time, double end_time, NonnullRefPtr<CSSStyleValue const> start_value, NonnullRefPtr<CSSStyleValue const> end_value,
    NonnullRefPtr<CSSStyleValue const> reversing_adjusted_start_value, double reversing_shortening_factor)
{
    auto& realm = element.realm();
    return realm.heap().allocate<CSSTransition>(realm, realm, element, property_id, transition_generation, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
}

Animations::AnimationClass CSSTransition::animation_class() const
{
    return Animations::AnimationClass::CSSTransition;
}

Optional<int> CSSTransition::class_specific_composite_order(JS::NonnullGCPtr<Animations::Animation> other_animation) const
{
    auto other = JS::NonnullGCPtr { verify_cast<CSSTransition>(*other_animation) };

    // Within the set of CSS Transitions, two animations A and B are sorted in composite order (first to last) as
    // follows:

    // 1. If neither A nor B has an owning element, sort based on their relative position in the global animation list.
    if (!owning_element() && !other->owning_element())
        return global_animation_list_order() - other->global_animation_list_order();

    // 2. Otherwise, if only one of A or B has an owning element, let the animation with an owning element sort first.
    if (owning_element() && !other->owning_element())
        return -1;
    if (!owning_element() && other->owning_element())
        return 1;

    // 3. Otherwise, if the owning element of A and B differs, sort A and B by tree order of their corresponding owning
    //    elements. With regard to pseudo-elements, the sort order is as follows:
    //    - element
    //    - ::marker
    //    - ::before
    //    - any other pseudo-elements not mentioned specifically in this list, sorted in ascending order by the Unicode
    //      codepoints that make up each selector
    //    - ::after
    //    - element children
    if (owning_element().ptr() != other->owning_element().ptr()) {
        // FIXME: Actually sort by tree order
        return {};
    }

    // 4. Otherwise, if A and B have different transition generation values, sort by their corresponding transition
    //    generation in ascending order.
    if (m_transition_generation != other->m_transition_generation)
        return m_transition_generation - other->m_transition_generation;

    // FIXME:
    // 5. Otherwise, sort A and B in ascending order by the Unicode codepoints that make up the expanded transition
    //    property name of each transition (i.e. without attempting case conversion and such that ‘-moz-column-width’
    //    sorts before ‘column-width’).
    return {};
}

CSSTransition::CSSTransition(JS::Realm& realm, DOM::Element& element, PropertyID property_id, size_t transition_generation,
    double start_time, double end_time, NonnullRefPtr<CSSStyleValue const> start_value, NonnullRefPtr<CSSStyleValue const> end_value,
    NonnullRefPtr<CSSStyleValue const> reversing_adjusted_start_value, double reversing_shortening_factor)
    : Animations::Animation(realm)
    , m_transition_property(property_id)
    , m_transition_generation(transition_generation)
    , m_start_time(start_time)
    , m_end_time(end_time)
    , m_start_value(move(start_value))
    , m_end_value(move(end_value))
    , m_reversing_adjusted_start_value(move(reversing_adjusted_start_value))
    , m_reversing_shortening_factor(reversing_shortening_factor)
    , m_keyframe_effect(Animations::KeyframeEffect::create(realm))
{
    // FIXME:
    // Transitions generated using the markup defined in this specification are not added to the global animation list
    // when they are created. Instead, these animations are appended to the global animation list at the first moment
    // when they transition out of the idle play state after being disassociated from their owning element. Transitions
    // that have been disassociated from their owning element but are still idle do not have a defined composite order.

    set_start_time(start_time - element.document().timeline()->current_time().value());

    // Construct a KeyframesEffect for our animation
    m_keyframe_effect->set_target(&element);
    m_keyframe_effect->set_start_delay(start_time);
    m_keyframe_effect->set_iteration_duration(m_end_time - start_time);
    m_keyframe_effect->set_timing_function(element.property_transition_attributes(property_id)->timing_function);

    auto key_frame_set = adopt_ref(*new Animations::KeyframeEffect::KeyFrameSet);
    Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame initial_keyframe;
    initial_keyframe.properties.set(property_id, m_start_value);

    Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame final_keyframe;
    final_keyframe.properties.set(property_id, m_end_value);

    key_frame_set->keyframes_by_key.insert(0, initial_keyframe);
    key_frame_set->keyframes_by_key.insert(100 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor, final_keyframe);

    m_keyframe_effect->set_key_frame_set(key_frame_set);
    set_timeline(element.document().timeline());
    set_owning_element(element);
    set_effect(m_keyframe_effect);
    element.associate_with_animation(*this);
    element.set_transition(m_transition_property, *this);

    HTML::TemporaryExecutionContext context(element.document().relevant_settings_object());
    play().release_value_but_fixme_should_propagate_errors();
}

void CSSTransition::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSTransition);
}

void CSSTransition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_declaration);
    visitor.visit(m_keyframe_effect);
}

double CSSTransition::timing_function_output_at_time(double t) const
{
    auto progress = (t - transition_start_time()) / (transition_end_time() - transition_start_time());
    // FIXME: Is this before_flag value correct?
    bool before_flag = t < transition_start_time();
    return m_keyframe_effect->timing_function().evaluate_at(progress, before_flag);
}

NonnullRefPtr<CSSStyleValue const> CSSTransition::value_at_time(double t) const
{
    // https://drafts.csswg.org/css-transitions/#application
    auto progress = timing_function_output_at_time(t);
    auto result = interpolate_property(*m_keyframe_effect->target(), m_transition_property, m_start_value, m_end_value, progress);
    if (result)
        return result.release_nonnull();
    return m_start_value;
}

}
