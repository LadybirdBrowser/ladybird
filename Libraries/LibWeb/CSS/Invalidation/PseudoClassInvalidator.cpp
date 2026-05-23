/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Invalidation/AncestorTraversal.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>

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

    Vector<CSS::InvalidationSet::Property, 1> properties { { CSS::InvalidationSet::Property::Type::PseudoClass, pseudo_class } };
    DOM::StyleInvalidationOptions options { .invalidate_self = true };
    auto reason = DOM::StyleInvalidationReason::PseudoClassStateChange;

    auto invalidate = [&](DOM::Element& element) {
        element.invalidate_style(reason, properties, options);

        // The interaction-state pseudo classes (Hover/Focus/etc.) aren't tracked in
        // pseudo_classes_used_in_has_selectors, so invalidate_node_style_for_properties
        // doesn't schedule :has() ancestor invalidation for them. Schedule it directly so
        // rules like .a:has(:focus) ... re-evaluate when the state flips.
        element.for_each_style_scope_which_may_observe_the_node([&](CSS::StyleScope& scope) {
            if (!scope.may_have_has_selectors())
                return;
            scope.record_pending_has_invalidation_mutation_features(element, properties);
            scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(element);
        });
    };

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
    auto walk_and_invalidate = [&](GC::Ptr<DOM::Node> start, HashTable<DOM::Element const*> const& other_chain) {
        if (!start)
            return;
        if (propagates) {
            for_each_inclusive_ancestor_element(*start, traversal, [&](DOM::Element& element) {
                if (other_chain.contains(&element))
                    return TraversalDecision::Break;
                invalidate(element);
                return TraversalDecision::Continue;
            });
        } else if (auto* element = as_if<DOM::Element>(*start)) {
            if (!other_chain.contains(element))
                invalidate(*element);
        }
    };

    walk_and_invalidate(old_state, new_chain);
    walk_and_invalidate(new_state, old_chain);
}

}
