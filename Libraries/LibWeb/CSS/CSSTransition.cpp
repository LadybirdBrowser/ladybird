/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/StyleEngineRustFFI.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSTransition);

GC::Ref<CSSTransition> CSSTransition::start_a_provisional_transition(
    DOM::AbstractElement abstract_element,
    PropertyID property_id,
    size_t transition_generation,
    double delay,
    double start_time,
    double end_time,
    NonnullRefPtr<StyleValue const> start_value,
    NonnullRefPtr<StyleValue const> end_value,
    NonnullRefPtr<StyleValue const> reversing_adjusted_start_value,
    double reversing_shortening_factor,
    EasingFunction timing_function)
{
    auto& environment = abstract_element.document().relevant_settings_object();
    return GC::Heap::the().allocate<CSSTransition>(environment, abstract_element, property_id, transition_generation, delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor, move(timing_function));
}

Utf16FlyString const& CSSTransition::transition_property() const
{
    return string_from_property_id(m_transition_property);
}

Animations::AnimationClass CSSTransition::animation_class() const
{
    return Animations::AnimationClass::CSSTransition;
}

int CSSTransition::class_specific_composite_order(GC::Ref<Animations::Animation> other_animation) const
{
    auto other = GC::Ref { as<CSSTransition>(*other_animation) };

    // Within the set of CSS Transitions, two animations A and B are sorted in composite order (first to last) as
    // follows:

    // 1. If neither A nor B has an owning element, sort based on their relative position in the global animation list.
    if (!owning_element().has_value() && !other->owning_element().has_value())
        return global_animation_list_order() - other->global_animation_list_order();

    // 2. Otherwise, if only one of A or B has an owning element, let the animation with an owning element sort first.
    if (owning_element().has_value() && !other->owning_element().has_value())
        return -1;
    if (!owning_element().has_value() && other->owning_element().has_value())
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
    if (owning_element() != other->owning_element()) {
        // FIXME: Actually sort by tree order
        return 0;
    }

    // 4. Otherwise, if A and B have different transition generation values, sort by their corresponding transition
    //    generation in ascending order.
    if (m_transition_generation != other->m_transition_generation)
        return m_transition_generation - other->m_transition_generation;

    // 5. Otherwise, sort A and B in ascending order by the Unicode codepoints that make up the expanded transition
    //    property name of each transition (i.e. without attempting case conversion and such that ‘-moz-column-width’
    //    sorts before ‘column-width’).
    return transition_property() <=> other->transition_property();
}

CSSTransition::CSSTransition(
    HTML::EnvironmentSettingsObject& environment,
    DOM::AbstractElement abstract_element,
    PropertyID property_id,
    size_t transition_generation,
    double delay,
    double start_time,
    double end_time,
    NonnullRefPtr<StyleValue const> start_value,
    NonnullRefPtr<StyleValue const> end_value,
    NonnullRefPtr<StyleValue const> reversing_adjusted_start_value,
    double reversing_shortening_factor,
    EasingFunction timing_function)
    : Animations::Animation(environment)
    , m_transition_property(property_id)
    , m_transition_generation(transition_generation)
    , m_start_time(start_time + delay)
    , m_end_time(end_time + delay)
    , m_start_value(move(start_value))
    , m_end_value(move(end_value))
    , m_reversing_adjusted_start_value(move(reversing_adjusted_start_value))
    , m_reversing_shortening_factor(reversing_shortening_factor)
    , m_keyframe_effect(Animations::KeyframeEffect::create())
{
    // FIXME:
    // Transitions generated using the markup defined in this specification are not added to the global animation list
    // when they are created. Instead, these animations are appended to the global animation list at the first moment
    // when they transition out of the idle play state after being disassociated from their owning element. Transitions
    // that have been disassociated from their owning element but are still idle do not have a defined composite order.

    // Construct a KeyframesEffect for our animation
    // NB: The current style computation collects this effect before publishing its result, so scheduling a second
    //     animated style update here would evaluate the same transition twice.
    m_keyframe_effect->set_target(abstract_element, Animations::KeyframeEffect::InvalidateEffect::No);
    m_keyframe_effect->set_specified_start_delay(delay);
    m_keyframe_effect->set_specified_iteration_duration(end_time - start_time);
    // AD-HOC: CSS Transitions require the start value to apply during transition-delay. A default KeyframeEffect does
    //         not fill in the before phase, so use backwards fill to keep the transition value in the cascade until
    //         the active interval starts.
    m_keyframe_effect->set_fill_mode(Animations::FillMode::Backwards);
    // https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
    // Timing properties may also be updated due to a style change. Any change to a CSS animation property that affects
    // timing requires rerunning the procedure to normalize specified timing.
    m_keyframe_effect->normalize_specified_timing();
    m_keyframe_effect->set_timing_function(move(timing_function));

    auto key_frame_set = adopt_ref(*new Animations::KeyframeEffect::KeyFrameSet);
    Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame initial_keyframe;
    initial_keyframe.properties.set(PropertyNameAndID::from_id(property_id), RustStyleValueHandle::retained(m_start_value->rust_style_value_data()));

    Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame final_keyframe;
    final_keyframe.properties.set(PropertyNameAndID::from_id(property_id), RustStyleValueHandle::retained(m_end_value->rust_style_value_data()));

    key_frame_set->keyframes_by_key.insert(0, initial_keyframe);
    key_frame_set->keyframes_by_key.insert(100 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor, final_keyframe);

    m_keyframe_effect->set_key_frame_set(key_frame_set);
    set_timeline(abstract_element.document().timeline());
    set_owning_element(abstract_element);
    set_provisional_effect(m_keyframe_effect);

    HTML::TemporaryExecutionContext context(environment);
    play(Animations::Animation::ShouldInvalidate::No).release_value_but_fixme_should_propagate_errors();
}

void CSSTransition::commit_provisional_transition()
{
    VERIFY(m_is_provisional);
    auto target = m_keyframe_effect->target();
    VERIFY(target);
    auto owner = owning_element();
    VERIFY(owner.has_value());
    target->associate_with_animation(*this);
    target->set_transition(owner->pseudo_element(), m_transition_property, *this);
    m_is_provisional = false;
}

void CSSTransition::discard_provisional_transition()
{
    VERIFY(m_is_provisional);
    set_timeline({});
    discard_provisional_effect();
    m_is_provisional = false;
}

// The identity the style engine describes the effect of `transition` by, or 0.
static u64 effect_identity(CSSTransition const& transition)
{
    auto effect = transition.effect();
    if (!effect || !effect->is_keyframe_effect())
        return 0;
    return static_cast<Animations::KeyframeEffect const&>(*effect).animation_preparation_identity();
}

void CSSTransition::adopt_lane_transitions(DOM::Document& document)
{
    auto* arena = document.layout_node_arena_if_created();
    if (!arena)
        return;
    auto& style_computer = document.style_computer();
    auto adopt_value = [](StyleValueFFI::StyleValueData const* value) {
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value));
    };
    auto adopt = [&](u32 style_node, ReadonlySpan<u64> cancelled, ReadonlySpan<u64> seen, ReadonlySpan<StyleValueFFI::FfiLaneTransition> lane_transitions) {
        auto element = style_computer.element_for_lane_style_node(StyleNodeID { style_node });
        if (!element || !element->is_connected())
            return;
        // A transition the lane started that the host took in already runs from when the lane started it.
        auto runs_as_lane_does = [&](CSSTransition const& transition, PropertyID property_id) {
            return any_of(lane_transitions, [&](auto const& lane_transition) {
                if (lane_transition.property_id != to_underlying(property_id))
                    return false;
                if (lane_transition.host_identity != 0)
                    return lane_transition.host_identity == effect_identity(transition);
                return transition.m_adopted_from_lane && transition.transition_start_time() == lane_transition.start_time + lane_transition.delay;
            });
        };
        // The transitions of the host's the lane's steps ended as they ran end as they did, and so does one the lane
        // started that the host took in and the lane no longer runs. One the steps saw end by itself ends as it does,
        // with its events. One the host started since the lane forked is its own: the lane decided nothing over it, and
        // the host decides what becomes of it, and of its property, in its turn. So does one script finished meanwhile.
        Vector<GC::Ref<CSSTransition>> ended;
        Vector<PropertyID> host_decides;
        if (auto const* existing = element->existing_transitions({})) {
            for (auto const& [property_id, transition] : *existing) {
                if (transition->is_idle() || transition->is_finished() || runs_as_lane_does(transition, property_id))
                    continue;
                auto identity = effect_identity(transition);
                if (cancelled.contains_slow(identity)) {
                    ended.append(transition);
                } else if (seen.contains_slow(identity)) {
                    continue;
                } else if (transition->m_adopted_from_lane) {
                    ended.append(transition);
                } else {
                    host_decides.append(property_id);
                }
            }
        }
        for (auto& transition : ended) {
            transition->cancel();
            element->remove_transition({}, transition->m_transition_property);
        }
        // A transition of the host's whose play is still pending runs from when the lane took it to start.
        for (auto const& lane_transition : lane_transitions) {
            if (lane_transition.host_identity == 0)
                continue;
            auto transition = element->property_transition({}, static_cast<PropertyID>(lane_transition.property_id));
            if (transition && effect_identity(*transition) == lane_transition.host_identity && !transition->start_time().has_value()
                && transition->pending() && transition->play_state() != Bindings::AnimationPlayState::Paused)
                (void)transition->set_start_time_for_bindings(Animations::NullableCSSNumberish { lane_transition.start_time });
        }
        for (auto const& lane_transition : lane_transitions) {
            if (lane_transition.host_identity != 0)
                continue;
            auto property_id = static_cast<PropertyID>(lane_transition.property_id);
            if (host_decides.contains_slow(property_id))
                continue;
            if (auto current = element->property_transition({}, property_id)) {
                if (runs_as_lane_does(*current, property_id))
                    continue;
                // A completed transition of the property gives way to the one the lane started. One the lane saw end,
                // which the host's timeline has yet to reach, ends now.
                if (!current->is_idle() && !current->is_finished())
                    (void)current->finish();
                element->remove_transition({}, property_id);
            }
            auto transition = start_a_provisional_transition(*element, property_id, document.transition_generation(),
                lane_transition.delay, lane_transition.start_time, lane_transition.start_time + lane_transition.duration,
                adopt_value(lane_transition.start_value), adopt_value(lane_transition.end_value), adopt_value(lane_transition.reversing_adjusted_start_value),
                lane_transition.reversing_shortening_factor, EasingFunction::from_style_value(adopt_value(lane_transition.timing_function)));
            transition->commit_provisional_transition();
            transition->m_adopted_from_lane = true;
            (void)transition->set_start_time_for_bindings(Animations::NullableCSSNumberish { lane_transition.start_time });
        }
    };
    // Starting and cancelling transitions settles their promises in the document's realm. The rendering update runs
    // their reactions once every document took in what its lanes did.
    HTML::TemporaryExecutionContext execution_context { document.relevant_settings_object() };
    StyleEngineFFI::style_engine_adopt_lane_transitions(
        arena->host(), &adopt,
        [](void* context, u32 style_node, u64 const* cancelled, size_t cancelled_count, u64 const* seen, size_t seen_count, StyleValueFFI::FfiLaneTransition const* transitions, size_t transition_count) {
            (*static_cast<decltype(adopt)*>(context))(style_node, { cancelled, cancelled_count }, { seen, seen_count }, { transitions, transition_count });
        });
}

void CSSTransition::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_declaration);
    visitor.visit(m_keyframe_effect);
}

double CSSTransition::timing_function_output_at_time(double t) const
{
    // AD-HOC: If the transition has an empty duration then we get NaN here,
    // setting progress to 1 because an instant transition may be considered "finished".
    double progress = 1;
    if (transition_start_time() < transition_end_time())
        progress = (t - transition_start_time()) / (transition_end_time() - transition_start_time());

    // FIXME: Is this before_flag value correct?
    bool before_flag = t < transition_start_time();
    return m_keyframe_effect->timing_function().evaluate_at(progress, before_flag);
}

}
