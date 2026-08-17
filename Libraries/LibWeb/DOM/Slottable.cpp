/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationObserver.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>

namespace Web::DOM {

SlottableMixin::~SlottableMixin() = default;

void SlottableMixin::RareData::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(assigned_slot);
    visitor.visit(manual_slot_assignment);
}

Utf16FlyString const& SlottableMixin::slottable_name() const
{
    static Utf16FlyString const empty_name;
    auto const* rare_data = slottable_rare_data();
    return rare_data ? rare_data->name : empty_name;
}

void SlottableMixin::set_slottable_name(Utf16FlyString name)
{
    if (name.is_empty()) {
        if (auto* rare_data = slottable_rare_data())
            rare_data->name = Utf16FlyString {};
        return;
    }
    ensure_slottable_rare_data().name = move(name);
}

GC::Ptr<HTML::HTMLSlotElement> SlottableMixin::assigned_slot_internal() const
{
    auto const* rare_data = slottable_rare_data();
    return rare_data ? rare_data->assigned_slot : nullptr;
}

void SlottableMixin::set_assigned_slot(GC::Ptr<HTML::HTMLSlotElement> assigned_slot)
{
    if (!assigned_slot) {
        if (auto* rare_data = slottable_rare_data())
            rare_data->assigned_slot = nullptr;
        return;
    }
    ensure_slottable_rare_data().assigned_slot = assigned_slot;
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

    // The assignedSlot getter steps are to return the result of find a slot given this and with the open flag set.
    return find_a_slot(node.as_slottable(), OpenFlag::Set);
}

GC::Ptr<HTML::HTMLSlotElement> assigned_slot_for_node(GC::Ref<Node> node)
{
    if (!node->is_slottable())
        return nullptr;

    return node->as_slottable().visit([](auto const& slottable) {
        return slottable->assigned_slot_internal();
    });
}

// https://dom.spec.whatwg.org/#slotable-assigned
bool is_an_assigned_slottable(GC::Ref<Node> node)
{
    if (!node->is_slottable())
        return false;

    // A slottable is assigned if its assigned slot is non-null.
    return assigned_slot_for_node(node) != nullptr;
}

// https://dom.spec.whatwg.org/#find-a-slot
GC::Ptr<HTML::HTMLSlotElement> find_a_slot(Slottable const& slottable, OpenFlag open_flag)
{
    // 1. If slottable’s parent is null, then return null.
    auto parent = slottable.visit([](auto& node) { return node->parent_element(); });
    if (!parent)
        return nullptr;

    // 2. Let shadow be slottable’s parent’s shadow root.
    auto shadow = parent->shadow_root();

    // 3. If shadow is null, then return null.
    if (shadow == nullptr)
        return nullptr;

    // 4. If the open flag is set and shadow’s mode is not "open", then return null.
    if (open_flag == OpenFlag::Set && shadow->mode() != Web::DOM::ShadowRootMode::Open)
        return nullptr;

    // 5. If shadow’s slot assignment is "manual", then return the slot in shadow’s descendants whose manually assigned
    //    nodes contains slottable, if any; otherwise null.
    if (shadow->slot_assignment() == Web::DOM::SlotAssignmentMode::Manual) {
        GC::Ptr<HTML::HTMLSlotElement> slot;

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
    return shadow->first_slot_with_name(slottable_name.view());
}

// https://dom.spec.whatwg.org/#find-slotables
Vector<Slottable> find_slottables(GC::Ref<HTML::HTMLSlotElement> slot)
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
    if (shadow_root.slot_assignment() == Web::DOM::SlotAssignmentMode::Manual) {
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

// https://dom.spec.whatwg.org/#find-flattened-slotables
Vector<Slottable> find_flattened_slottables(GC::Ref<HTML::HTMLSlotElement> slot)
{
    // 1. Let result be « ».
    Vector<Slottable> result;

    // 2. If slot’s root is not a shadow root, then return result.
    if (!slot->root().is_shadow_root())
        return result;

    // 3. Let slottables be the result of finding slottables given slot.
    auto slottables = find_slottables(slot);

    // 4. If slottables is the empty list, then append each slottable child of slot, in tree order, to slottables.
    if (slottables.is_empty()) {
        slot->for_each_child([&](auto& node) {
            if (!node.is_slottable())
                return IterationDecision::Continue;

            slottables.append(node.as_slottable());
            return IterationDecision::Continue;
        });
    }

    // 5. For each node of slottables:
    for (auto& node : slottables) {
        // 1. If node is a slot whose root is a shadow root:
        //     1. Let temporaryResult be the result of finding flattened slottables given node.
        //     2. Append each slottable in temporaryResult, in order, to result.
        // 2. Otherwise, append node to result.
        auto* maybe_slot = node.get_pointer<GC::Ref<DOM::Element>>();
        if (!maybe_slot) {
            result.append(node);
            continue;
        }

        if (auto* slot = as_if<HTML::HTMLSlotElement>(maybe_slot->ptr()); slot && slot->root().is_shadow_root()) {
            auto temporary_result = find_flattened_slottables(*slot);
            result.extend(temporary_result);
        } else {
            result.append(node);
        }
    }

    // 6. Return result.
    return result;
}

// https://dom.spec.whatwg.org/#assign-slotables
void assign_slottables(GC::Ref<HTML::HTMLSlotElement> slot)
{
    // 1. Let slottables be the result of finding slottables for slot.
    auto slottables = find_slottables(slot);

    // 2. If slottables and slot’s assigned nodes are not identical, then run signal a slot change for slot.
    auto assignment_changed = slottables != slot->assigned_nodes_internal();
    if (assignment_changed)
        signal_a_slot_change(slot);

    // AD-HOC: Clear the assigned slot for slottables that are no longer assigned to this slot.
    //         This must happen before setting the new assigned slots to avoid stale references
    //         during style computation. A tree-wide assignment can already have moved one of the
    //         old slottables to another slot, which the old slot must not overwrite when it is
    //         processed afterward.
    for (auto const& old_slottable : slot->assigned_nodes_internal()) {
        if (old_slottable.visit([](auto const& node) { return node->assigned_slot_internal(); }) != slot)
            continue;
        if (slottables.contains_slow(old_slottable))
            continue;
        old_slottable.visit([&](auto const& node) {
            node->set_assigned_slot(nullptr);
        });
        if (auto const* element = old_slottable.template get_pointer<GC::Ref<Element>>())
            CSS::record_element_assigned_slot_changed(**element, slot.ptr());
    }

    // 4. For each slottable in slottables, set slottable’s assigned slot to slot.
    for (auto& slottable : slottables) {
        auto old_slot = slottable.visit([](auto const& node) { return node->assigned_slot_internal(); });
        if (old_slot == slot)
            continue;
        slottable.visit([&](auto& node) {
            node->set_assigned_slot(slot);
        });
        if (auto* element = slottable.template get_pointer<GC::Ref<Element>>())
            CSS::record_element_assigned_slot_changed(**element, old_slot.ptr());
    }

    // 3. Set slot’s assigned nodes to slottables.
    // NOTE: We do this step last so that we can move the slottables list.
    slot->set_assigned_nodes(move(slottables));

    if (assignment_changed) {
        slot->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::SlotAssignmentChange);
        CSS::Invalidation::invalidate_style_after_slot_assignment_change(slot);
    }
}

// https://dom.spec.whatwg.org/#assign-slotables-for-a-tree
void assign_slottables_for_a_tree(GC::Ref<Node> root)
{
    // To assign slottables for a tree, given a node root, run assign slottables for each slot of root’s inclusive
    // descendants, in tree order.

    // OPTIMIZATION: If root is not a shadow root or slot element, there are no slots to process, so return early.
    auto* shadow_root = as_if<ShadowRoot>(*root);
    if (!shadow_root && !root->is_html_slot_element())
        return;

    if (shadow_root) {
        shadow_root->for_each_registered_slot([](HTML::HTMLSlotElement& slot) {
            assign_slottables(slot);
        });
        return;
    }

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
void signal_a_slot_change(GC::Ref<HTML::HTMLSlotElement> slottable)
{
    // 1. Append slot to slot’s relevant agent’s signal slots.
    // NB: signal_slots is supposed to be a set, so ensure the slottable is not already there.
    auto& signal_slots = HTML::relevant_similar_origin_window_agent(slottable).signal_slots;
    if (!signal_slots.contains_slow(slottable))
        signal_slots.append(slottable);

    // 2. Queue a mutation observer microtask.
    queue_mutation_observer_microtask(HTML::relevant_similar_origin_window_agent(slottable));
}

}
