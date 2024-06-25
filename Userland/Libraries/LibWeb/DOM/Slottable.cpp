/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::DOM {

SlottableMixin::~SlottableMixin() = default;

void SlottableMixin::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_assigned_slot);
    visitor.visit(m_manual_slot_assignment);
}

// https://dom.spec.whatwg.org/#dom-slotable-assignedslot
JS::GCPtr<HTML::HTMLSlotElement> SlottableMixin::assigned_slot()
{
    auto* node = dynamic_cast<DOM::Node*>(this);
    VERIFY(node);

    // The assignedSlot getter steps are to return the result of find a slot given this and with the open flag set.
    return find_a_slot(node->as_slottable(), OpenFlag::Set);
}

JS::GCPtr<HTML::HTMLSlotElement> assigned_slot_for_node(JS::NonnullGCPtr<Node> node)
{
    if (!node->is_slottable())
        return nullptr;

    return node->as_slottable().visit([](auto const& slottable) {
        return slottable->assigned_slot_internal();
    });
}

// https://dom.spec.whatwg.org/#slotable-assigned
bool is_an_assigned_slottable(JS::NonnullGCPtr<Node> node)
{
    if (!node->is_slottable())
        return false;

    // A slottable is assigned if its assigned slot is non-null.
    return assigned_slot_for_node(node) != nullptr;
}

// https://dom.spec.whatwg.org/#find-a-slot
JS::GCPtr<HTML::HTMLSlotElement> find_a_slot(Slottable const& slottable, OpenFlag open_flag)
{
    // 1. If slottable’s parent is null, then return null.
    auto* parent = slottable.visit([](auto& node) { return node->parent_element(); });
    if (!parent)
        return nullptr;

    // 2. Let shadow be slottable’s parent’s shadow root.
    auto shadow = parent->shadow_root();

    // 3. If shadow is null, then return null.
    if (shadow == nullptr)
        return nullptr;

    // 4. If the open flag is set and shadow’s mode is not "open", then return null.
    if (open_flag == OpenFlag::Set && shadow->mode() != Bindings::ShadowRootMode::Open)
        return nullptr;

    // 5. If shadow’s slot assignment is "manual", then return the slot in shadow’s descendants whose manually assigned
    //    nodes contains slottable, if any; otherwise null.
    if (shadow->slot_assignment() == Bindings::SlotAssignmentMode::Manual) {
        JS::GCPtr<HTML::HTMLSlotElement> slot;

        shadow->for_each_in_subtree_of_type<HTML::HTMLSlotElement>([&](auto& child) {
            if (!child.manually_assigned_nodes().contains_slow(slottable))
                return TraversalDecision::Continue;

            slot = child;
            return TraversalDecision::Break;
        });

        return slot;
    }

    // 6. Return the first slot in tree order in shadow’s descendants whose name is slottable’s name, if any; otherwise null.
    auto const& slottable_name = slottable.visit([](auto const& node) { return node->slottable_name(); });
    JS::GCPtr<HTML::HTMLSlotElement> slot;

    shadow->for_each_in_subtree_of_type<HTML::HTMLSlotElement>([&](auto& child) {
        if (child.slot_name() != slottable_name)
            return TraversalDecision::Continue;

        slot = child;
        return TraversalDecision::Break;
    });

    return slot;
}

// https://dom.spec.whatwg.org/#find-slotables
Vector<Slottable> find_slottables(JS::NonnullGCPtr<HTML::HTMLSlotElement> slot)
{
    // 1. Let result be an empty list.
    Vector<Slottable> result;

    // 2. Let root be slot’s root.
    auto& root = slot->root();

    // 3. If root is not a shadow root, then return result.
    if (!root.is_shadow_root())
        return result;

    // 4. Let host be root’s host.
    auto& shadow_root = static_cast<ShadowRoot&>(root);
    auto* host = shadow_root.host();

    // 5. If root’s slot assignment is "manual", then:
    if (shadow_root.slot_assignment() == Bindings::SlotAssignmentMode::Manual) {
        // 1. Let result be « ».
        // 2. For each slottable slottable of slot’s manually assigned nodes, if slottable’s parent is host, append slottable to result.
        for (auto const& slottable : slot->manually_assigned_nodes()) {
            auto const* parent = slottable.visit([](auto const& node) { return node->parent(); });

            if (parent == host)
                result.append(slottable);
        }
    }
    // 6. Otherwise, for each slottable child slottable of host, in tree order:
    else {
        host->for_each_child([&](auto& node) {
            if (!node.is_slottable())
                return IterationDecision::Continue;

            auto slottable = node.as_slottable();

            // 1. Let foundSlot be the result of finding a slot given slottable.
            auto found_slot = find_a_slot(slottable);

            // 2. If foundSlot is slot, then append slottable to result.
            if (found_slot == slot)
                result.append(move(slottable));

            return IterationDecision::Continue;
        });
    }

    // 7. Return result.
    return result;
}

// https://dom.spec.whatwg.org/#assign-slotables
void assign_slottables(JS::NonnullGCPtr<HTML::HTMLSlotElement> slot)
{
    // 1. Let slottables be the result of finding slottables for slot.
    auto slottables = find_slottables(slot);

    // 2. If slottables and slot’s assigned nodes are not identical, then run signal a slot change for slot.
    if (slottables != slot->assigned_nodes_internal())
        signal_a_slot_change(slot);

    // 4. For each slottable in slottables, set slottable’s assigned slot to slot.
    for (auto& slottable : slottables) {
        slottable.visit([&](auto& node) {
            node->set_assigned_slot(slot);
        });
    }

    // 3. Set slot’s assigned nodes to slottables.
    // NOTE: We do this step last so that we can move the slottables list.
    slot->set_assigned_nodes(move(slottables));
}

// https://dom.spec.whatwg.org/#assign-slotables-for-a-tree
void assign_slottables_for_a_tree(JS::NonnullGCPtr<Node> root)
{
    // AD-HOC: This method iterates over the root's entire subtree. That iteration does nothing if the root is not a
    //         shadow root (see `find_slottables`). This iteration can be very expensive as the HTML parser inserts
    //         nodes, especially on sites with many elements. So we skip it if we know it's going to be a no-op anyways.
    if (!root->is_shadow_root())
        return;

    // To assign slottables for a tree, given a node root, run assign slottables for each slot slot in root’s inclusive
    // descendants, in tree order.
    root->for_each_in_inclusive_subtree_of_type<HTML::HTMLSlotElement>([](auto& slot) {
        assign_slottables(slot);
        return TraversalDecision::Continue;
    });
}

// https://dom.spec.whatwg.org/#assign-a-slot
void assign_a_slot(Slottable const& slottable)
{
    // 1. Let slot be the result of finding a slot with slottable.
    auto slot = find_a_slot(slottable);

    // 2. If slot is non-null, then run assign slottables for slot.
    if (slot != nullptr)
        assign_slottables(*slot);
}

// https://dom.spec.whatwg.org/#signal-a-slot-change
void signal_a_slot_change(JS::NonnullGCPtr<HTML::HTMLSlotElement> slottable)
{
    // FIXME: 1. Append slot to slot’s relevant agent’s signal slots.

    // 2. Queue a mutation observer microtask.
    Bindings::queue_mutation_observer_microtask(slottable->document());
}

}
