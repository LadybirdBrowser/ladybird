/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/HasMutationFeatureCollector.h>
#include <LibWeb/CSS/Invalidation/HasMutationInvalidator.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS::Invalidation {

static bool reason_may_affect_has_selectors(DOM::StyleInvalidationReason reason)
{
    // :has() selectors match based on DOM state only (structure, attributes, pseudo-classes). Reasons that don't change
    // any DOM state can't affect :has() matching, so we can skip scheduling :has() ancestor invalidation.
    switch (reason) {
    case DOM::StyleInvalidationReason::BaseURLChanged:
    case DOM::StyleInvalidationReason::CSSFontLoaded:
    case DOM::StyleInvalidationReason::HTMLIFrameElementGeometryChange:
    case DOM::StyleInvalidationReason::HTMLObjectElementUpdateLayoutAndChildObjects:
    case DOM::StyleInvalidationReason::NavigableSetViewportSize:
    case DOM::StyleInvalidationReason::SettingsChange:
        return false;
    default:
        return true;
    }
}

static void invalidate_children_affected_by_has_sibling_combinators(DOM::Node& parent)
{
    parent.for_each_child_of_type<DOM::Element>([&](auto& element) {
        if (element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator())
            element.invalidate_style_if_affected_by_has();
        return IterationDecision::Continue;
    });
}

static void schedule_has_invalidation_for_child_list_mutation(DOM::Node& parent, DOM::Node& mutation_root, StyleScope& scope)
{
    if (!scope.may_have_has_selectors())
        return;

    auto has_sibling_combinator_has_selectors = scope.may_have_has_selectors_with_relative_selector_that_has_sibling_combinator();

    // Sibling-combinator :has() selectors are sensitive to featureless insertions/removals because a plain node can
    // still change adjacency and following-sibling relationships.
    auto may_affect_has_match = mutation_root.is_character_data()
        || subtree_has_feature_used_in_has_selector(mutation_root, scope)
        || has_sibling_combinator_has_selectors;
    if (!may_affect_has_match)
        return;

    scope.record_pending_has_invalidation_mutation_features(parent, mutation_root, true);
    scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(parent);

    if (has_sibling_combinator_has_selectors)
        invalidate_children_affected_by_has_sibling_combinators(parent);
}

void schedule_has_invalidation_for_node(DOM::Node& node, DOM::StyleInvalidationReason reason)
{
    auto is_child_list_mutation = reason == DOM::StyleInvalidationReason::NodeRemove
        || reason == DOM::StyleInvalidationReason::NodeInsertBefore;

    // On insertion and removal the mutated node itself is uninteresting to the
    // :has() walker (a freshly inserted node has no :has() scope flags yet, and
    // a removed node is about to leave the tree). Start the walk at the parent,
    // which was in scope before and reliably carries the correct flags.
    if (is_child_list_mutation) {
        auto* parent = node.parent_or_shadow_host();
        if (!parent)
            return;

        // Walk every scope that can observe the parent, including enclosing and hosted shadow roots, so :has() in
        // :host(), ::slotted(), and ::part() selectors can react to the mutation.
        parent->for_each_style_scope_which_may_observe_the_node([&](StyleScope& scope) {
            schedule_has_invalidation_for_child_list_mutation(*parent, node, scope);
        });
        return;
    }

    auto& style_scope = node.style_scope();
    if (!style_scope.may_have_has_selectors() || !reason_may_affect_has_selectors(reason))
        return;
    style_scope.record_pending_has_invalidation_mutation_features(node, node, false);
    style_scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(node);
}

void schedule_has_invalidation_for_same_parent_move(DOM::Node& node)
{
    auto* parent = node.parent_or_shadow_host();
    if (!parent)
        return;

    parent->for_each_style_scope_which_may_observe_the_node([&](StyleScope& scope) {
        schedule_has_invalidation_for_child_list_mutation(*parent, node, scope);
    });
}

}
