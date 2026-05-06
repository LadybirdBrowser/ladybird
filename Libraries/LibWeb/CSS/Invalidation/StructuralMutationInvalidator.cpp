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

    // OPTIMIZATION: For :first-child / :last-child / :only-child, the match result only flips for
    //               at most one element per insertion/removal, so we can target it precisely instead
    //               of invalidating every previous/next sibling that ever observed those pseudo-classes.
    // NB: :first-child / :last-child match on element siblings (skipping text/comment nodes), so we
    //     find the adjacent *element* sibling here rather than the adjacent node.
    GC::Ptr<DOM::Element> last_child_transition_target;
    GC::Ptr<DOM::Element> first_child_transition_target;
    if (is_insertion_or_removal) {
        // For NodeInsertBefore this runs after the insertion; for NodeRemove it runs before the
        // removal. In both cases the node is currently in the tree, so the element siblings reflect
        // the state in which "this node is at the trailing/leading element position" → the adjacent
        // element is the one whose :last-child / :first-child match transitions.
        GC::Ptr<DOM::Element> previous_element;
        for (auto* sibling = node.previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
            if (auto* element = as_if<DOM::Element>(sibling)) {
                previous_element = element;
                break;
            }
        }
        GC::Ptr<DOM::Element> next_element;
        for (auto* sibling = node.next_sibling(); sibling; sibling = sibling->next_sibling()) {
            if (auto* element = as_if<DOM::Element>(sibling)) {
                next_element = element;
                break;
            }
        }
        if (!next_element && previous_element)
            last_child_transition_target = previous_element;
        if (!previous_element && next_element)
            first_child_transition_target = next_element;
    }

    if (is_insertion_or_removal) {
        // OPTIMIZATION: Only walk previous siblings if the parent has been observed to contain a child that matches a
        //               pseudo-class whose match result can depend on siblings after that element. Otherwise, no
        //               previous sibling can possibly need invalidation due to this insertion or removal.
        if (auto* parent_node = as_if<DOM::ParentNode>(node.parent()); parent_node && parent_node->has_child_affected_by_backward_structural_changes()) {
            auto& counters = node.document().style_invalidation_counters();
            for (auto* sibling = node.previous_sibling(); sibling; sibling = sibling->previous_sibling()) {
                ++counters.previous_sibling_invalidation_walk_visits;
                auto* element = as_if<DOM::Element>(sibling);
                if (!element)
                    continue;
                bool needs_mark = false;
                if (element->affected_by_backward_positional_pseudo_class()) {
                    // :nth-last-child / :nth-last-of-type / :last-of-type / :only-of-type all need
                    // every previous sibling re-evaluated since their from-end indices shift.
                    needs_mark = true;
                } else if (element->affected_by_last_child_pseudo_class() && element == last_child_transition_target) {
                    // :last-child and :only-child only flip for the element transitioning into/out of
                    // the trailing position.
                    needs_mark = true;
                }
                if (needs_mark)
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
            if (element->affected_by_indirect_sibling_combinator()) {
                needs_to_invalidate = true;
            } else if (element->affected_by_forward_positional_pseudo_class()) {
                // :nth-child / :nth-of-type / :first-of-type / :only-of-type need every next sibling
                // re-evaluated since their leading indices shift.
                needs_to_invalidate = true;
            } else if (element->affected_by_first_child_pseudo_class() && element == first_child_transition_target) {
                // :first-child / :only-child only flip for the element transitioning into/out of the
                // leading position.
                needs_to_invalidate = true;
            } else if (element->affected_by_direct_sibling_combinator() && current_sibling_distance <= element->sibling_invalidation_distance()) {
                needs_to_invalidate = true;
            }
        } else if (element->affected_by_indirect_sibling_combinator()) {
            needs_to_invalidate = true;
        } else if (element->affected_by_forward_positional_pseudo_class()) {
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
