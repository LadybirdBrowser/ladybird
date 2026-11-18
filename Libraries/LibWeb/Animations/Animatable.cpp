/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Animatable.h"
#include <LibJS/HostClassBuilder.h>
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
void web_animation_for_each_associated_animation(JSObject* node_wrapper, bool subtree, void* context, void (*on_animation)(void* context, RustFFI::AnimationCell const*));
i32 web_animation_compare_elements_in_tree_order(void* first, void* second);
}

// The associated animations of `element`, and of its descendants where `subtree` is set.
static void for_each_associated_animation(DOM::Element const& element, bool subtree, Function<void(AnimationRef const&)> const& callback)
{
    for (auto const& record : element.associated_animations_unordered())
        callback(record);

    if (subtree) {
        element.for_each_child_of_type<DOM::Element>([&](auto const& child) {
            for_each_associated_animation(child, true, callback);
            return IterationDecision::Continue;
        });
    }
}

// NB: The Rust implementations of getAnimations() collect the associated animations of an element, or of the subtree
//     of a node where `subtree` is set, with this, after the pending style changes are applied, and keep the relevant
//     ones and sort them themselves.
void web_animation_for_each_associated_animation(JSObject* node_wrapper, bool subtree, void* context, void (*on_animation)(void* context, RustFFI::AnimationCell const*))
{
    auto& node = *Bindings::impl_from<DOM::Node>(JS::HostABI::object_from_abi(node_wrapper));
    node.document().update_style();
    Function<void(AnimationRef const&)> push = [&](AnimationRef const& animation) { on_animation(context, &animation.record()); };
    if (auto const* element = as_if<DOM::Element>(node)) {
        for_each_associated_animation(*element, subtree, push);
        return;
    }
    VERIFY(subtree);
    as<DOM::ParentNode>(node).for_each_child_of_type<DOM::Element>([&](auto const& child) {
        for_each_associated_animation(child, true, push);
        return IterationDecision::Continue;
    });
}

i32 web_animation_compare_elements_in_tree_order(void* first, void* second)
{
    return compare_elements_in_tree_order_for_composite_order(first, second);
}

Optional<RustFFI::AnimationOwnerSlot> Animatable::owner_slot(Optional<CSS::PseudoElement> pseudo_element)
{
    if (!pseudo_element.has_value())
        return RustFFI::AnimationOwnerSlot { 0 };
    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(*pseudo_element))
        return {};
    return RustFFI::AnimationOwnerSlot { static_cast<u8>(to_underlying(*pseudo_element) + 1) };
}

Animatable::~Animatable()
{
    finalize_animations();
}

void Animatable::finalize_animations()
{
    if (auto* animations = exchange(m_animations, nullptr))
        RustFFI::animations_element_animations_destroy(animations, as_if<DOM::Element>(*this));
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

size_t Animatable::animation_record_memory_size(bool (*first_counted)(void*, RustFFI::KeyframeSet const*), void* context) const
{
    return m_animations ? RustFFI::animations_element_animations_record_memory_size(m_animations, first_counted, context) : 0;
}

void Animatable::associate_with_animation(AnimationRef const& animation)
{
    auto& element = as<DOM::Element>(*this);
    if (!RustFFI::animations_element_animations_associate(&ensure_animations(), &element.document().document_animations(), &animation.record(), element.document().is_in_style_stabilization_epoch()))
        return;
    // The style engine computes no record for an element whose animations compose its style.
    CSS::record_element_animations_changed(element);

    element.change_associated_animation_count_in_subtree(1);
    CSS::record_element_associated_with_animation(element);
}

void Animatable::disassociate_with_animation(AnimationRef const& animation, Optional<EffectRef> const& effect)
{
    auto& element = as<DOM::Element>(*this);
    auto was_associated = RustFFI::animations_element_animations_disassociate(m_animations, &element.document().document_animations(), &animation.record(), effect.has_value() ? &effect->record() : nullptr);
    CSS::record_element_animations_changed(element);

    if (was_associated)
        element.change_associated_animation_count_in_subtree(-1);
}

void Animatable::on_document_changed(DOM::Document& old_document, DOM::Document& new_document)
{
    if (m_animations)
        RustFFI::animations_element_animations_move_to_document(m_animations, &old_document.document_animations(), &new_document.document_animations(), new_document.is_in_style_stabilization_epoch());
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
        m_animations, style_engine_host_of(*this), style_node_of(*this), keep_animation_alive_in, in_execution_context_of_animation, &animations, web_animation_perform_host_effect);
}

void Animatable::cancel_css_animations_and_transitions_of(CSS::PseudoElement pseudo_element)
{
    auto slot = owner_slot(pseudo_element);
    if (!m_animations || !slot.has_value())
        return;

    GC::RootVector<AnimationRef> animations;
    RustFFI::animations_element_animations_cancel_css_animations_and_transitions_of_owner(
        m_animations, *slot, style_engine_host_of(*this), style_node_of(*this), keep_animation_alive_in, in_execution_context_of_animation, &animations, web_animation_perform_host_effect);
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
        &context, web_animation_perform_host_effect);
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
    auto slot = owner_slot(pseudo_element);
    return m_animations && slot.has_value() && RustFFI::animations_element_animations_count(m_animations, RustFFI::ElementAnimationListKind::Transitions, *slot) != 0;
}

void Animatable::visit_edges(JS::Cell::Visitor& visitor)
{
    if (!m_animations)
        return;
    RustFFI::animations_element_animations_for_each_host_cell(m_animations, &visitor, visit_host_cell);
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

}
