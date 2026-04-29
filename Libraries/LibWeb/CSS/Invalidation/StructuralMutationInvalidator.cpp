/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/HasMutationInvalidator.h>
#include <LibWeb/CSS/Invalidation/StructuralMutationInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS::Invalidation {

static bool element_is_affected_by_same_parent_move(DOM::Element const& element)
{
    return element.affected_by_forward_structural_changes()
        || element.affected_by_backward_structural_changes()
        || element.affected_by_has_pseudo_class_in_subject_position()
        || element.affected_by_has_pseudo_class_in_non_subject_position()
        || element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator();
}

static void mark_sibling_for_style_update(DOM::Element& element)
{
    auto mark_descendant_shadow_roots_for_style_update = [](DOM::Element& element) {
        element.for_each_shadow_including_inclusive_descendant([](DOM::Node& inclusive_descendant) {
            auto* descendant_element = as_if<DOM::Element>(inclusive_descendant);
            if (!descendant_element)
                return TraversalDecision::Continue;
            auto shadow_root = descendant_element->shadow_root();
            if (!shadow_root)
                return TraversalDecision::Continue;
            shadow_root->set_entire_subtree_needs_style_update(true);
            shadow_root->set_needs_style_update(true);
            return TraversalDecision::Continue;
        });
    };

    // The structural change might flip whether `element` matches selectors like :nth-child or `~`/`+`, but those
    // pseudo-classes and combinators only affect `element` itself unless some descendant selector also depends on
    // `element`'s position. Mark just `element` when descendants don't observe its position; mark the entire
    // subtree only when they do.
    if (element.affected_by_structural_pseudo_class_in_non_subject_position() || element.affected_by_sibling_combinator_in_non_subject_position()) {
        element.set_entire_subtree_needs_style_update(true);
    } else {
        element.set_needs_style_update(true);
        mark_descendant_shadow_roots_for_style_update(element);
    }
}

void invalidate_structurally_affected_siblings(DOM::Node& node, DOM::StyleInvalidationReason reason)
{
    auto is_insertion_or_removal = reason == DOM::StyleInvalidationReason::NodeInsertBefore
        || reason == DOM::StyleInvalidationReason::NodeRemove;

    if (is_insertion_or_removal) {
        // OPTIMIZATION: Only walk previous siblings if the parent has been observed to contain a child that matches a
        //               pseudo-class whose match result can depend on siblings after that element. Otherwise, no
        //               previous sibling can possibly need invalidation due to this insertion or removal.
        if (auto* parent_node = as_if<DOM::ParentNode>(node.parent()); parent_node && parent_node->has_child_affected_by_backward_structural_changes()) {
            auto& counters = node.document().style_invalidation_counters();
            for (auto* sibling = node.previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
                ++counters.previous_sibling_invalidation_walk_visits;
                if (auto* element = as_if<DOM::Element>(sibling); element && element->affected_by_backward_structural_changes())
                    mark_sibling_for_style_update(*element);
            }
        }
    }

    size_t current_sibling_distance = 1;
    for (auto* sibling = node.next_sibling(); sibling; sibling = sibling->next_sibling()) {
        auto* element = as_if<DOM::Element>(sibling);
        if (!element)
            continue;

        bool needs_to_invalidate = false;
        if (is_insertion_or_removal) {
            if (element->affected_by_indirect_sibling_combinator() || element->affected_by_first_child_pseudo_class() || element->affected_by_forward_positional_pseudo_class())
                needs_to_invalidate = true;
            else if (element->affected_by_direct_sibling_combinator() && current_sibling_distance <= element->sibling_invalidation_distance())
                needs_to_invalidate = true;
        } else if (element->affected_by_indirect_sibling_combinator() || element->affected_by_forward_positional_pseudo_class()) {
            needs_to_invalidate = true;
        } else if (element->affected_by_direct_sibling_combinator() && current_sibling_distance <= element->sibling_invalidation_distance()) {
            needs_to_invalidate = true;
        }

        if (needs_to_invalidate)
            mark_sibling_for_style_update(*element);
        current_sibling_distance++;
    }
}

void mark_ancestors_as_having_child_needing_style_update(DOM::Node& node)
{
    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host())
        ancestor->set_child_needs_style_update(true);
}

void invalidate_style_after_same_parent_move(DOM::Node& node, DOM::StyleInvalidationReason reason)
{
    if (node.document().needs_full_style_update())
        return;

    schedule_has_invalidation_for_same_parent_move(node);

    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->entire_subtree_needs_style_update())
            return;
    }

    if (auto* element = as_if<DOM::Element>(node)) {
        if (element->affected_by_structural_pseudo_class_in_non_subject_position() || element->affected_by_sibling_combinator_in_non_subject_position()) {
            node.set_entire_subtree_needs_style_update(true);
        } else if (element_is_affected_by_same_parent_move(*element)) {
            node.set_needs_style_update(true);
        }
    } else if (!node.is_character_data()) {
        node.set_needs_style_update(true);
    }
    invalidate_structurally_affected_siblings(node, reason);
    mark_ancestors_as_having_child_needing_style_update(node);
}

}
