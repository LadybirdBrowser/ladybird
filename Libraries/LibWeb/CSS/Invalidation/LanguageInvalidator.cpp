/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::CSS::Invalidation {

// `lang` and `dir` both inherit, so a change on one element changes what every element under it
// resolves to. Each of them publishes the value it now has, and the rules that name a language or a
// direction reach their subjects from that rather than from the walk.
static void publish_language_and_directionality(DOM::Element& element, bool is_directionality_change)
{
    element.for_each_shadow_including_inclusive_descendant([is_directionality_change](auto& node) {
        if (auto* descendant = as_if<DOM::Element>(node)) {
            if (is_directionality_change) {
                record_element_directionality(*descendant);
            } else {
                descendant->invalidate_lang_value();
                record_element_language_and_directionality(*descendant);
            }
            return TraversalDecision::Continue;
        }
        if (is_directionality_change)
            return TraversalDecision::Continue;
        // Rendered text under a casing text-transform is keyed on the language
        // (locale-sensitive casing), which no computed style group reflects, so a style
        // recomputation alone never re-renders it.
        auto* text_layout_node = as_if<Layout::TextNode>(node.unsafe_layout_node());
        if (!text_layout_node || !text_layout_node->parent())
            return TraversalDecision::Continue;
        auto text_transform = text_layout_node->parent()->text_transform();
        if (!first_is_one_of(text_transform, TextTransform::Uppercase, TextTransform::Lowercase, TextTransform::Capitalize))
            return TraversalDecision::Continue;
        if (is<Layout::TextSliceNode>(*text_layout_node)) {
            // NB: Slice nodes cache data that is calculated at layout tree construction time,
            //     and locale-sensitive casing can move the first-letter boundary.
            if (node.parent())
                node.parent()->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::LanguageChangeUnderCasingTextTransform);
        } else {
            text_layout_node->invalidate_text_for_rendering();
            text_layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::LanguageChangeUnderCasingTextTransform);
        }
        return TraversalDecision::Continue;
    });
}

void invalidate_style_after_language_change(DOM::Element& element)
{
    publish_language_and_directionality(element, false);
}

static void publish_directionality_dependent_ancestors(DOM::Element& element, HashTable<DOM::Element*>& visited)
{
    for (auto ancestor = GC::Ptr<DOM::Element> { element }; ancestor; ancestor = ancestor->parent_element()) {
        if (visited.set(ancestor.ptr()) != AK::HashSetResult::InsertedNewEntry)
            continue;
        if (ancestor->has_auto_directionality())
            publish_language_and_directionality(*ancestor, true);

        if (auto assigned_slot = ancestor->assigned_slot_internal())
            publish_directionality_dependent_ancestors(*assigned_slot, visited);
    }
}

void invalidate_style_after_directionality_change(DOM::Element& element)
{
    publish_language_and_directionality(element, true);
    HashTable<DOM::Element*> visited;
    visited.set(&element);
    if (auto parent = element.parent_element())
        publish_directionality_dependent_ancestors(*parent, visited);
    if (auto assigned_slot = element.assigned_slot_internal())
        publish_directionality_dependent_ancestors(*assigned_slot, visited);
}

void invalidate_style_after_slot_assignment_change(HTML::HTMLSlotElement& slot)
{
    HashTable<DOM::Element*> visited;
    publish_directionality_dependent_ancestors(slot, visited);
}

// dir=auto resolves an element's effective directionality from the text under it, so text arriving,
// leaving, or changing its data can flip it. Every ancestor holding dir=auto republishes what it now
// resolves to, for its whole subtree, because a directionality inherits.
void invalidate_style_after_text_change_under(DOM::Element& parent_of_text)
{
    bool ancestor_chain_has_assigned_slot = false;
    for (auto ancestor = GC::Ptr<DOM::Element> { parent_of_text }; ancestor; ancestor = ancestor->parent_element()) {
        if (ancestor->assigned_slot_internal()) {
            ancestor_chain_has_assigned_slot = true;
            break;
        }
    }

    if (!ancestor_chain_has_assigned_slot) {
        for (auto ancestor = GC::Ptr<DOM::Element> { parent_of_text }; ancestor; ancestor = ancestor->parent_element()) {
            if (ancestor->has_auto_directionality())
                publish_language_and_directionality(*ancestor, true);
        }
        return;
    }

    HashTable<DOM::Element*> visited;
    publish_directionality_dependent_ancestors(parent_of_text, visited);
}

}
