/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::CSS::Invalidation {

void invalidate_assigned_slottables_after_slot_style_change(DOM::Element& element)
{
    // Slotted elements inherit from their assigned slot in the flat tree, but they are DOM children of the shadow
    // host, so the normal DOM tree walk won't propagate inherited style changes to them.
    auto* slot = as_if<HTML::HTMLSlotElement>(element);
    if (!slot)
        return;
    for (auto const& slottable : slot->assigned_nodes_internal()) {
        if (auto const* assigned_element = slottable.get_pointer<GC::Ref<DOM::Element>>())
            (*assigned_element)->document().style_computer().style_engine().record_element_style_input_change((*assigned_element)->style_node_id());
    }
}

}
