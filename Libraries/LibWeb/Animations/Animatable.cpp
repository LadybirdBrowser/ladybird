/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Animatable.h"
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::Animations {

extern "C" {

// Called from Bindings/Rust/src/animations/animatable.rs.
void web_animation_for_each_relevant_animation(JSObject* node_wrapper, bool subtree, void* context, void (*on_animation)(void* context, RustFFI::AnimationCell const*));
i32 web_animation_compare_elements_in_tree_order(void* first, void* second);
}

void Animatable::for_each_relevant_animation(IncludeSubtree include_subtree, Function<void(AnimationRef const&)> const& callback) const
{
    for (auto const& record : associated_animations_unordered()) {
        // AD-HOC: SVG animation elements drive their animations with their own timing model, which script must not
        //         be able to play, pause or cancel. Like other engines, do not expose them.
        if (record.timing_state().kind == RustFFI::FfiAnimationKind::Smil)
            continue;
        if (record.is_relevant())
            callback(record);
    }

    if (include_subtree == IncludeSubtree::Yes) {
        static_cast<DOM::Element const*>(this)->for_each_child_of_type<DOM::Element>([&](auto const& child) {
            child.for_each_relevant_animation(IncludeSubtree::Yes, callback);
            return IterationDecision::Continue;
        });
    }
}

// NB: The Rust implementations of getAnimations() collect the relevant animations of an element, or of the subtree of a
//     node where `subtree` is set, with this, after the pending style changes are applied, and sort them themselves.
void web_animation_for_each_relevant_animation(JSObject* node_wrapper, bool subtree, void* context, void (*on_animation)(void* context, RustFFI::AnimationCell const*))
{
    auto& node = *Bindings::impl_from<DOM::Node>(JS::HostABI::object_from_abi(node_wrapper));
    node.document().update_style();
    auto include_subtree = subtree ? Animatable::IncludeSubtree::Yes : Animatable::IncludeSubtree::No;
    Function<void(AnimationRef const&)> push = [&](AnimationRef const& animation) { on_animation(context, &animation.record()); };
    if (auto const* element = as_if<DOM::Element>(node)) {
        element->for_each_relevant_animation(include_subtree, push);
        return;
    }
    VERIFY(include_subtree == Animatable::IncludeSubtree::Yes);
    as<DOM::ParentNode>(node).for_each_child_of_type<DOM::Element>([&](auto const& child) {
        child.for_each_relevant_animation(Animatable::IncludeSubtree::Yes, push);
        return IterationDecision::Continue;
    });
}

i32 web_animation_compare_elements_in_tree_order(void* first, void* second)
{
    return compare_elements_in_tree_order_for_composite_order(first, second);
}

// The list of the element, or of one of its known pseudo-elements, of the CSS animations and transitions it owns.
static Optional<RustFFI::AnimationOwnerSlot> owner_slot(Optional<CSS::PseudoElement> pseudo_element)
{
    if (!pseudo_element.has_value())
        return RustFFI::AnimationOwnerSlot { 0 };
    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(*pseudo_element))
        return {};
    return RustFFI::AnimationOwnerSlot { static_cast<u8>(to_underlying(*pseudo_element) + 1) };
}

static RustFFI::FfiTransitionProperty transition_property(CSS::PropertyNameAndID const& property)
{
    return {
        .property_id = to_underlying(property.id()),
        .custom_name = property.is_custom_property() ? property.name().raw_identity() : 0,
    };
}

Animatable::~Animatable()
{
    finalize_animations();
}

void Animatable::finalize_animations()
{
    if (auto* animations = exchange(m_animations, nullptr))
        RustFFI::animations_element_animations_destroy(animations);
}

RustFFI::ElementAnimations& Animatable::ensure_animations()
{
    if (!m_animations)
        m_animations = RustFFI::animations_element_animations_create();
    return *m_animations;
}

void Animatable::forget_published_effect_descriptions()
{
    if (m_animations)
        RustFFI::animations_element_animations_forget_published_effect_descriptions(m_animations);
}

ElementAnimationList Animatable::associated_animations_in_composite_order()
{
    if (m_animations)
        RustFFI::animations_element_animations_sort_in_composite_order(m_animations, compare_elements_in_tree_order_for_composite_order);
    return associated_animations_unordered();
}

ElementAnimationList Animatable::associated_animations_unordered() const
{
    return { m_animations, RustFFI::ElementAnimationListKind::Associated, RustFFI::AnimationOwnerSlot { 0 } };
}

void Animatable::invalidate_associated_animation_composite_order()
{
    if (m_animations)
        RustFFI::animations_element_animations_invalidate_composite_order(m_animations);
}

bool Animatable::has_associated_animations() const
{
    return !associated_animations_unordered().is_empty();
}

size_t Animatable::animation_lists_memory_size() const
{
    return m_animations ? RustFFI::animations_element_animations_memory_size(m_animations) : 0;
}

bool Animatable::has_relevant_animations() const
{
    for (auto const& animation : associated_animations_unordered()) {
        if (animation.is_relevant())
            return true;
    }
    return false;
}

void Animatable::associate_with_animation(AnimationRef const& animation)
{
    if (!RustFFI::animations_element_animations_associate(&ensure_animations(), &animation.record()))
        return;
    // The style engine computes no record for an element whose animations compose its style.
    CSS::record_element_animations_changed(as<DOM::Element>(*this));

    as<DOM::Element>(*this).change_associated_animation_count_in_subtree(1);
    CSS::record_element_associated_with_animation(as<DOM::Element>(*this));

    as<DOM::Element>(*this).document().associate_with_animation(animation);
}

void Animatable::disassociate_with_animation(AnimationRef const& animation, Optional<EffectRef> const& effect)
{
    auto was_associated = m_animations && RustFFI::animations_element_animations_disassociate(m_animations, &animation.record());
    CSS::record_element_animations_changed(as<DOM::Element>(*this));

    if (was_associated)
        as<DOM::Element>(*this).change_associated_animation_count_in_subtree(-1);

    as<DOM::Element>(*this).document().disassociate_with_animation(animation, effect);
}

void Animatable::on_document_changed(DOM::Document& old_document, DOM::Document& new_document)
{
    for (auto const& animation : associated_animations_unordered()) {
        old_document.disassociate_with_animation(animation, animation.effect());
        new_document.associate_with_animation(animation);
    }
}

// The computation of an element's animation definitions matches them against the animations the element already has,
// so the lists publish the name of each and the definition it last had applied to the style engine, which knows the
// element by its identity there, as they change.
static Layout::RustFFI::DocumentHost const* style_engine_host_of(Animatable& animatable)
{
    auto* element = as_if<DOM::Element>(animatable);
    return element ? CSS::style_engine_host_for_identified(*element) : nullptr;
}

static u32 style_node_of(Animatable& animatable)
{
    auto* element = as_if<DOM::Element>(animatable);
    return element ? element->style_node_id().value() : 0;
}

void Animatable::cancel_css_animations_and_transitions()
{
    if (!m_animations)
        return;

    GC::RootVector<AnimationRef> animations;
    RustFFI::animations_element_animations_cancel_css_animations_and_transitions(
        m_animations, style_engine_host_of(*this), style_node_of(*this), keep_animation_alive_in,
        [](void*, RustFFI::AnimationCell const* animation, void (*run)(void*), void* run_context) {
            HTML::TemporaryExecutionContext execution_context { HTML::relevant_realm(relevant_global_object_of(AnimationRef { *animation })) };
            run(run_context);
        },
        &animations, perform_host_effect);
}

void Animatable::play_or_pause_css_animations_as_their_play_state_asks()
{
    if (!m_animations)
        return;

    struct Context {
        GC::RootVector<AnimationRef> animations;
        GC::Ref<DOM::Element> element;
    } context { {}, as<DOM::Element>(*this) };
    RustFFI::animations_element_animations_play_or_pause_css_animations(
        m_animations,
        [](void* context, RustFFI::AnimationCell const* animation) {
            keep_animation_alive_in(&static_cast<Context*>(context)->animations, animation);
        },
        [](void* context, RustFFI::AnimationCell const*, void (*run)(void*), void* run_context) {
            HTML::TemporaryExecutionContext execution_context { static_cast<Context*>(context)->element->document().relevant_settings_object() };
            run(run_context);
        },
        &context, perform_host_effect);
}

bool Animatable::has_matching_transition_property_entry(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto const& style = static_cast<DOM::Element const&>(*this).installed_style(pseudo_element);
    if (!style)
        return false;
    auto view = style.view();
    return CSS::StyleEngineFFI::style_engine_view_has_matching_transition_entries(&view, has_existing_transitions(pseudo_element));
}

bool Animatable::has_existing_transitions(Optional<CSS::PseudoElement> pseudo_element) const
{
    return !existing_transitions(pseudo_element).is_empty();
}

ElementAnimationList Animatable::existing_transitions(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto slot = owner_slot(pseudo_element);
    return { slot.has_value() ? m_animations : nullptr, RustFFI::ElementAnimationListKind::Transitions, slot.value_or(0) };
}

Optional<AnimationRef> Animatable::property_transition(Optional<CSS::PseudoElement> pseudo_element, CSS::PropertyNameAndID const& property) const
{
    auto slot = owner_slot(pseudo_element);
    if (!m_animations || !slot.has_value())
        return {};
    auto const* transition = RustFFI::animations_element_animations_transition(m_animations, *slot, transition_property(property));
    if (!transition)
        return {};
    return AnimationRef { *transition };
}

void Animatable::commit_provisional_transition(Optional<CSS::PseudoElement> pseudo_element, AnimationRef const& transition, RustFFI::TransitionOrigin origin)
{
    // NB: The record associates the transition with this element, its target, and holds it here where this element or
    //     pseudo-element can own transitions.
    auto slot = owner_slot(pseudo_element);
    RustFFI::animations_css_transition_commit_provisional(&transition.record(), slot.has_value() ? &ensure_animations() : nullptr, slot.value_or(RustFFI::AnimationOwnerSlot { 0 }), origin, perform_host_effect);
}

void Animatable::remove_transition(Optional<CSS::PseudoElement> pseudo_element, CSS::PropertyNameAndID const& property)
{
    if (auto slot = owner_slot(pseudo_element); slot.has_value())
        RustFFI::animations_element_animations_remove_transition(&ensure_animations(), *slot, transition_property(property));
}

void Animatable::visit_edges(JS::Cell::Visitor& visitor)
{
    if (!m_animations)
        return;
    RustFFI::animations_element_animations_for_each_host_cell(m_animations, &visitor, visit_host_cell);
}

bool Animatable::can_own_css_animations_and_transitions(Optional<CSS::PseudoElement> pseudo_element)
{
    return owner_slot(pseudo_element).has_value();
}

bool Animatable::has_css_animations_or_transitions() const
{
    return m_animations && RustFFI::animations_element_animations_has_css_animations_or_transitions(m_animations);
}

bool Animatable::has_css_defined_animations(Optional<CSS::PseudoElement> pseudo_element) const
{
    auto slot = owner_slot(pseudo_element);
    return m_animations && slot.has_value() && RustFFI::animations_element_animations_count(m_animations, RustFFI::ElementAnimationListKind::CssAnimations, *slot) != 0;
}

GC::RootVector<AnimationRef> Animatable::css_defined_animations(Optional<CSS::PseudoElement> pseudo_element) const
{
    GC::RootVector<AnimationRef> animations;
    auto slot = owner_slot(pseudo_element);
    if (!slot.has_value())
        return animations;
    for (auto const& animation : ElementAnimationList { m_animations, RustFFI::ElementAnimationListKind::CssAnimations, *slot })
        animations.append(animation);
    return animations;
}

void Animatable::set_css_defined_animations(Optional<CSS::PseudoElement> pseudo_element, ReadonlySpan<AnimationRef> animations)
{
    auto slot = owner_slot(pseudo_element);
    if (!slot.has_value())
        return;
    // NB: Every element goes through this step; one that had no animations and still has none has nothing to hold or
    //     publish.
    if (!m_animations && animations.is_empty())
        return;

    Vector<RustFFI::AnimationCell const*, 1> records;
    records.ensure_capacity(animations.size());
    for (auto const& animation : animations)
        records.unchecked_append(&animation.record());
    RustFFI::animations_element_animations_set_css_animations(&ensure_animations(), *slot, records.span(), style_engine_host_of(*this), style_node_of(*this));
}

}
