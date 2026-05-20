/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_slottable_assignment_change(DOM::Slottable const& slottable)
{
    slottable.visit(
        [](GC::Ref<DOM::Element> const& element) {
            element->set_needs_style_update(true);
        },
        [](GC::Ref<DOM::Text> const&) {});
}

void invalidate_assigned_slottables_after_slot_style_change(DOM::Element& element)
{
    // Slotted elements inherit from their assigned slot in the flat tree, but they are DOM children of the shadow
    // host, so the normal DOM tree walk won't propagate inherited style changes to them.
    auto* slot = as_if<HTML::HTMLSlotElement>(element);
    if (!slot)
        return;
    for (auto const& slottable : slot->assigned_nodes_internal()) {
        slottable.visit([](auto const& assigned_node) {
            assigned_node->set_needs_style_update(true);
        });
    }
}

void invalidate_assigned_slottables_for_descendant_slots_after_inherited_style_change(DOM::Element& element)
{
    element.document().style_invalidation_counters().descendant_slot_invalidation_subtree_scans++;
    element.for_each_in_inclusive_subtree_of_type<HTML::HTMLSlotElement>([](HTML::HTMLSlotElement& slot) {
        for (auto const& slottable : slot.assigned_nodes_internal())
            invalidate_style_after_slottable_assignment_change(slottable);
        return TraversalDecision::Continue;
    });
}

}
