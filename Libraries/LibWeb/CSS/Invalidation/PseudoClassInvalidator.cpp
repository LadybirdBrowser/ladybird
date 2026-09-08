/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Invalidation/AncestorTraversal.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Invalidation {

static bool pseudo_class_propagates_to_ancestors(CSS::PseudoClass pseudo_class)
{
    return first_is_one_of(pseudo_class, CSS::PseudoClass::Hover, CSS::PseudoClass::FocusWithin);
}

static AncestorTraversal ancestor_traversal_for_pseudo_class(CSS::PseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case CSS::PseudoClass::FocusWithin:
        return AncestorTraversal::FlatTree;
    default:
        return AncestorTraversal::ShadowIncluding;
    }
}

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass pseudo_class, GC::Ptr<DOM::Node> old_state, GC::Ptr<DOM::Node> new_state)
{
    if (!old_state && !new_state)
        return;

    bool const propagates = pseudo_class_propagates_to_ancestors(pseudo_class);
    auto traversal = ancestor_traversal_for_pseudo_class(pseudo_class);

    auto build_chain = [&](GC::Ptr<DOM::Node> start) {
        HashTable<DOM::Element const*> chain;
        if (!start)
            return chain;
        if (propagates) {
            for_each_inclusive_ancestor_element(*start, traversal, [&](DOM::Element& element) {
                chain.set(&element);
                return TraversalDecision::Continue;
            });
        } else if (auto* element = as_if<DOM::Element>(*start)) {
            chain.set(element);
        }
        return chain;
    };

    auto old_chain = build_chain(old_state);
    auto new_chain = build_chain(new_state);

    // Walk start's ancestor chain (inclusive) and invalidate each element whose pseudo-class
    // state changes. Elements in both chains have unchanged state and are skipped; once we
    // reach one, all further ancestors are also in both chains so we stop.
    auto walk_and_invalidate = [&](GC::Ptr<DOM::Node> start, HashTable<DOM::Element const*> const& other_chain, bool new_value) {
        if (!start)
            return;
        // This walk visits exactly the elements whose state changed, which is exactly the set
        // StyleEngine has to hear a state fact for.
        auto visit = [&](DOM::Element& element) {
            // `:focus-visible` is not focus. It is focus the user agent has decided to indicate,
            // which the element answers for itself, so a focus move publishes what the element says
            // rather than the move. Publishing the move lights up every element script focuses.
            auto value = new_value;
            if (pseudo_class == CSS::PseudoClass::FocusVisible)
                value = value && element.should_indicate_focus();
            record_element_state_changed(element, pseudo_class, value);
        };
        if (propagates) {
            for_each_inclusive_ancestor_element(*start, traversal, [&](DOM::Element& element) {
                if (other_chain.contains(&element))
                    return TraversalDecision::Break;
                visit(element);
                return TraversalDecision::Continue;
            });
        } else if (auto* element = as_if<DOM::Element>(*start)) {
            if (!other_chain.contains(element))
                visit(*element);
        }
    };

    walk_and_invalidate(old_state, new_chain, false);
    walk_and_invalidate(new_state, old_chain, true);
}

struct PropagatingState {
    CSS::PseudoClass pseudo_class;
    GC::Ptr<DOM::Node> source;
};

static Array<PropagatingState, 2> propagating_states(DOM::Document& document)
{
    return { {
        { CSS::PseudoClass::FocusWithin, document.focused_area() },
        { CSS::PseudoClass::Hover, document.hovered_node() },
    } };
}

static bool subtree_holds_propagating_state_source(DOM::Node const& subtree, CSS::PseudoClass pseudo_class, DOM::Node const& source)
{
    if (ancestor_traversal_for_pseudo_class(pseudo_class) == AncestorTraversal::FlatTree) {
        for (auto const* node = &source; node; node = node->flat_tree_parent()) {
            if (node == &subtree)
                return true;
        }
        return false;
    }
    return subtree.is_shadow_including_inclusive_ancestor_of(source);
}

bool descendants_hold_a_propagating_state_source(DOM::Node& node)
{
    for (auto const& [pseudo_class, source] : propagating_states(node.document())) {
        if (source && source.ptr() != &node && subtree_holds_propagating_state_source(node, pseudo_class, *source))
            return true;
    }
    return false;
}

// A state that propagates to ancestors is a statement about the subtree below them, so moving a
// subtree that holds one moves the state: the chain above where it was stops holding it and the
// chain above where it landed starts, and no feature of any element in either chain moved to say
// so. The source has not moved relative to itself, so each element is asked its own answer rather
// than being told a transition.
void invalidate_style_after_subtree_place_changed(DOM::Node& subtree, GC::Ptr<DOM::Node> old_parent)
{
    auto republish = [](CSS::PseudoClass pseudo_class, GC::Ptr<DOM::Node> from) {
        if (!from)
            return;
        for_each_inclusive_ancestor_element(*from, ancestor_traversal_for_pseudo_class(pseudo_class), [&](DOM::Element& element) {
            auto* hovered = element.document().hovered_node();
            auto holds = pseudo_class == CSS::PseudoClass::FocusWithin
                ? element.matches_focus_within_pseudo_class()
                : hovered && (&element == hovered || element.is_shadow_including_ancestor_of(*hovered));
            record_element_state_changed(element, pseudo_class, holds);
            return TraversalDecision::Continue;
        });
    };

    for (auto const& [pseudo_class, source] : propagating_states(subtree.document())) {
        if (!source || !subtree_holds_propagating_state_source(subtree, pseudo_class, *source))
            continue;
        republish(pseudo_class, source);
        republish(pseudo_class, old_parent);
    }
}

}
