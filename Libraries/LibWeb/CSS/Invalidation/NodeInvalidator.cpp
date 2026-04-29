/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Invalidation/HasMutationInvalidator.h>
#include <LibWeb/CSS/Invalidation/NodeInvalidator.h>
#include <LibWeb/CSS/Invalidation/StructuralMutationInvalidator.h>
#include <LibWeb/CSS/Invalidation/StyleInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS::Invalidation {

[[maybe_unused]] static StringView to_string(DOM::StyleInvalidationReason reason)
{
#define __ENUMERATE_STYLE_INVALIDATION_REASON(reason) \
    case DOM::StyleInvalidationReason::reason:        \
        return #reason##sv;
    switch (reason) {
        ENUMERATE_STYLE_INVALIDATION_REASONS(__ENUMERATE_STYLE_INVALIDATION_REASON)
    default:
        VERIFY_NOT_REACHED();
    }
}

void invalidate_node_style(DOM::Node& node, DOM::StyleInvalidationReason reason)
{
    schedule_has_invalidation_for_node(node, reason);

    // Character data nodes have no style of their own, so once :has() ancestor invalidation has been scheduled there
    // is nothing else to do.
    if (node.is_character_data())
        return;

    if (!node.needs_style_update() && !node.document().needs_full_style_update())
        dbgln_if(STYLE_INVALIDATION_DEBUG, "Invalidate style ({}): {}", to_string(reason), node.debug_description());

    if (node.is_document()) {
        auto& document = static_cast<DOM::Document&>(node);
        document.record_full_style_invalidation();
        document.set_needs_full_style_update(true);
        return;
    }

    // If the document is already marked for a full style update, there's no need to do anything here.
    if (node.document().needs_full_style_update())
        return;

    // If any ancestor is already marked for an entire subtree update, there's no need to do anything here.
    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->entire_subtree_needs_style_update())
            return;
    }

    // When invalidating style for a node, we actually invalidate:
    // - the node itself
    // - all of its descendants
    // - preceding siblings that depend on following-sibling counts (only on DOM insert/remove)
    // - subsequent siblings that depend on previous siblings or sibling combinators
    // FIXME: This is a lot of invalidation and we should implement more sophisticated invalidation to do less work!

    node.set_entire_subtree_needs_style_update(true);

    invalidate_structurally_affected_siblings(node, reason);
    mark_ancestors_as_having_child_needing_style_update(node);
}

void invalidate_node_style_for_properties(DOM::Node& node, DOM::StyleInvalidationReason reason, Vector<CSS::InvalidationSet::Property> const& properties, DOM::StyleInvalidationOptions options)
{
    if (node.is_character_data())
        return;

    auto& style_scope = node.style_scope();

    // Collect every shadow scope this mutation can flip, in addition to the root scope. This includes:
    //   - The element's own shadow root (if it's a shadow host).
    //   - Every enclosing shadow host's shadow root, for :host(...:has(...)) and ::slotted(...) rules in those scopes
    //     that observe property changes on this element or its light-DOM children.
    //   - The document scope and any outer shadow root scopes when this element lives inside a shadow tree, for
    //     ::part(...:has(...)) rules in the outer document or containing shadow root.
    Vector<GC::Ref<CSS::StyleScope>, 4> additional_scopes;
    node.for_each_style_scope_which_may_observe_the_node([&](CSS::StyleScope& scope) {
        if (&scope == &style_scope)
            return;
        additional_scopes.append(scope);
    });

    bool properties_used_in_has_selectors = false;
    auto& counters = node.document().style_invalidation_counters();
    for (auto const& property : properties) {
        if (auto const* metadata = node.document().style_computer().has_invalidation_metadata_for_property(property, style_scope)) {
            properties_used_in_has_selectors = true;
            counters.has_invalidation_metadata_candidates += metadata->size();
        }
        for (auto& scope : additional_scopes) {
            if (auto const* metadata = node.document().style_computer().has_invalidation_metadata_for_property(property, scope)) {
                properties_used_in_has_selectors = true;
                counters.has_invalidation_metadata_candidates += metadata->size();
            }
        }
    }
    if (properties_used_in_has_selectors) {
        style_scope.record_pending_has_invalidation_mutation_features(node, properties);
        style_scope.schedule_ancestors_style_invalidation_due_to_presence_of_has(node);
        for (auto& scope : additional_scopes) {
            scope->record_pending_has_invalidation_mutation_features(node, properties);
            scope->schedule_ancestors_style_invalidation_due_to_presence_of_has(node);
        }
    }

    if (options.invalidate_self)
        node.set_needs_style_update(true);

    auto invalidate_for_style_scope = [&node, reason, &properties](CSS::StyleScope& style_scope) {
        auto plan = node.document().style_computer().invalidation_plan_for_properties(properties, style_scope);
        return node.document().style_invalidator().enqueue_invalidation_plan(node, reason, *plan);
    };

    if (invalidate_for_style_scope(style_scope))
        return;
    for (auto& scope : additional_scopes) {
        if (invalidate_for_style_scope(scope))
            return;
    }
}

}
