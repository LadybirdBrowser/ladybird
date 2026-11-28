/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Page/Page.h>

namespace Web::DOM {

SlottableMixin::~SlottableMixin() = default;

void SlottableMixin::RareData::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(manual_slot_assignment);
}

GC::Ptr<HTML::HTMLSlotElement> SlottableMixin::assigned_slot_internal() const
{
    return assigned_slot_for_node(const_cast<SlottableMixin&>(*this).slottable_as_node());
}

void SlottableMixin::set_assigned_slot(GC::Ptr<HTML::HTMLSlotElement> assigned_slot)
{
    if (assigned_slot_internal() == assigned_slot)
        return;
    auto& node = slottable_as_node();
    node.document().page().keyboard_scroll_dom_tree_changed(node);
    node.set_has_assigned_slot({}, assigned_slot != nullptr);
    if (auto* element = as_if<Element>(node))
        element->set_assigned_slot_in_core({}, assigned_slot.ptr());
    else
        as<Text>(node).set_assigned_slot_in_core({}, assigned_slot.ptr());
    if (node.document().may_have_blocking_wheel_event_listener())
        node.update_inside_blocking_wheel_event_handler_state_for_subtree();
    if (auto* element = as_if<Element>(node))
        node.document().note_element_assigned_slot_changed(*element);
}

GC::Ptr<HTML::HTMLSlotElement> SlottableMixin::manual_slot_assignment()
{
    auto const* rare_data = slottable_rare_data();
    return rare_data ? rare_data->manual_slot_assignment : nullptr;
}

void SlottableMixin::set_manual_slot_assignment(GC::Ptr<HTML::HTMLSlotElement> manual_slot_assignment)
{
    if (!manual_slot_assignment) {
        if (auto* rare_data = slottable_rare_data())
            rare_data->manual_slot_assignment = nullptr;
        return;
    }
    ensure_slottable_rare_data().manual_slot_assignment = manual_slot_assignment;
}

// https://dom.spec.whatwg.org/#dom-slotable-assignedslot
GC::Ptr<HTML::HTMLSlotElement> SlottableMixin::assigned_slot()
{
    auto& node = slottable_as_node();

    // The assignedSlot getter steps are to return the result of find a slot given this and true.
    // NB: libweb_rust finds the slot, see Rust/src/dom/slots.rs.
    return static_cast<HTML::HTMLSlotElement*>(const_cast<Node*>(RustFFI::rust_dom_find_a_slot(&node, true)));
}

}
