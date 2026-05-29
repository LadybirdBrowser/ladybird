/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Invalidation/AncestorTraversal.h>
#include <LibWeb/CSS/Invalidation/InvalidationSetMatcher.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/AbstractElement.h>
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

static bool pseudo_class_subject_may_match_element(DOM::Element& element, CSS::Selector const& selector, CSS::PseudoClass pseudo_class)
{
    auto const& compound_selectors = selector.compound_selectors();
    for (size_t i = compound_selectors.size(); i > 0; --i) {
        auto const& compound_selector = compound_selectors[i - 1];
        bool contains_pseudo_class = false;
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoClass
                && simple_selector.pseudo_class().type == pseudo_class) {
                contains_pseudo_class = true;
                break;
            }
        }
        if (!contains_pseudo_class)
            continue;

        if (!compound_may_match_element(element, compound_selector, pseudo_class))
            return false;

        if (compound_selector.simple_selectors.size() > 1 || i == 1)
            return true;

        if (compound_selector.combinator != CSS::Selector::Combinator::None)
            return true;

        // Some selectors are represented with the pseudo-class in its own
        // compound. In that case, the preceding compound carries the subject
        // constraints, such as the `a` in `a:hover`.
        return compound_may_match_element(element, compound_selectors[i - 2], pseudo_class);
    }
    return true;
}

static bool element_may_match_rule_containing_pseudo_class_in_style_scope(DOM::Element& element, CSS::StyleScope& style_scope, CSS::PseudoClass pseudo_class)
{
    bool may_match = false;
    auto abstract_element = DOM::AbstractElement { element };
    style_scope.get_pseudo_class_rule_cache(pseudo_class).for_each_matching_rules(abstract_element, [&](auto const& matching_rules) {
        for (auto const& matching_rule : matching_rules) {
            if (pseudo_class_subject_may_match_element(element, matching_rule.selector, pseudo_class)) {
                may_match = true;
                return IterationDecision::Break;
            }
        }
        return IterationDecision::Continue;
    });
    return may_match;
}

static bool element_may_match_rule_containing_pseudo_class(DOM::Element& element, CSS::PseudoClass pseudo_class)
{
    bool may_match = false;
    element.for_each_style_scope_which_may_observe_the_node([&](CSS::StyleScope& scope) {
        if (element_may_match_rule_containing_pseudo_class_in_style_scope(element, scope, pseudo_class))
            may_match = true;
    });
    return may_match;
}

void invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass pseudo_class, GC::Ptr<DOM::Node> old_state, GC::Ptr<DOM::Node> new_state)
{
    if (!old_state && !new_state)
        return;

    bool const propagates = pseudo_class_propagates_to_ancestors(pseudo_class);
    auto traversal = ancestor_traversal_for_pseudo_class(pseudo_class);

    Vector<CSS::InvalidationSet::Property, 1> properties { { CSS::InvalidationSet::Property::Type::PseudoClass, pseudo_class } };
    auto reason = DOM::StyleInvalidationReason::PseudoClassStateChange;

    auto invalidate = [&](DOM::Element& element) {
        DOM::StyleInvalidationOptions options {
            .invalidate_self = element_may_match_rule_containing_pseudo_class(element, pseudo_class),
        };
        options.invalidate_self_from_property_plan = options.invalidate_self;
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
