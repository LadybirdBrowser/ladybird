/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/InvalidationJournal.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::CSS::Invalidation {

// Where any of the text is cased by its language, the root lays out again.
static void enroll_language_dependent_text(Layout::Node& root)
{
    Layout::RustFFI::render_state_enroll_text_after_language_change(root.document_host(), Layout::Node::slot_id(&root));
}

void enroll_text_after_language_change(Layout::BegunRead const& read, DOM::Element& element)
{
    // Language is a DOM input outside the computed style groups. Rust checks the styles of every slice and refreshes
    // their text without rebuilding the source ranges, which depend on the untransformed text.
    element.for_each_shadow_including_inclusive_descendant([&read](auto& node) {
        if (auto* descendant = as_if<DOM::Element>(node)) {
            descendant->for_each_synthetic_pseudo_element([&read](CSS::PseudoElement, DOM::SyntheticPseudoElement const& pseudo) {
                if (auto* layout_node = pseudo.unsafe_layout_node(read))
                    enroll_language_dependent_text(*layout_node);
            });
        } else if (auto* text_layout_node = as_if<Layout::TextNode>(node.unsafe_layout_node(read))) {
            enroll_language_dependent_text(*text_layout_node);
        }
        return TraversalDecision::Continue;
    });
}

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
        }
        return TraversalDecision::Continue;
    });
    // The text under the element lays out again as the boxes are next drained, which may be beside a frame in flight.
    if (!is_directionality_change)
        element.document().invalidation_journal().note_language_changed(element);
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
    if (!parent_of_text.document().has_element_with_auto_directionality())
        return;

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
