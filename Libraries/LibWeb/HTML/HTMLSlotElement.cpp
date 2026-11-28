/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/HTMLSlotElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/RootedNodeList.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLSlotElement);

HTMLSlotElement::HTMLSlotElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    // NB: libweb_rust reads a slot as the HTMLSlotElementPrefix it starts with.
    static_assert(sizeof(HTMLSlotElement) == sizeof(DOM::RustFFI::HTMLSlotElementPrefix));
    static_assert(offsetof(HTMLSlotElement, m_slot_core) == offsetof(DOM::RustFFI::HTMLSlotElementPrefix, core));

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(HTMLSlotElement, m_slot_core) >= sizeof(HTMLElement));
}

HTMLSlotElement::~HTMLSlotElement()
{
    DOM::RustFFI::rust_dom_slot_core_destroy(&m_slot_core);
}

static HTMLSlotElement::AssignedNodesFlatten assigned_nodes_flatten(Bindings::AssignedNodesOptions const& options)
{
    return options.flatten ? HTMLSlotElement::AssignedNodesFlatten::Yes : HTMLSlotElement::AssignedNodesFlatten::No;
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-slot-assignednodes
Vector<GC::Root<DOM::Node>> HTMLSlotElement::assigned_nodes(AssignedNodesFlatten flatten) const
{
    // 1. If options["flatten"] is false, then return this's assigned nodes.
    if (flatten == AssignedNodesFlatten::No) {
        assign_slottables_if_needed();
        Vector<GC::Root<DOM::Node>> assigned_nodes;
        assigned_nodes.ensure_capacity(assigned_nodes_internal().size());
        for (auto const& node : assigned_nodes_internal())
            assigned_nodes.unchecked_append(*node);
        return assigned_nodes;
    }

    // 2. Return the result of finding flattened slottables with this.
    // NB: libweb_rust finds them, see Rust/src/dom/slots.rs.
    DOM::RootedNodeList nodes;
    DOM::RustFFI::rust_dom_find_flattened_slottables(this, &nodes.core());
    Vector<GC::Root<DOM::Node>> assigned_nodes;
    assigned_nodes.ensure_capacity(nodes.nodes().size());
    for (auto const& node : nodes.nodes())
        assigned_nodes.unchecked_append(*node);
    return assigned_nodes;
}

Vector<GC::Root<DOM::Node>> HTMLSlotElement::assigned_nodes(Bindings::AssignedNodesOptions const& options) const
{
    return assigned_nodes(assigned_nodes_flatten(options));
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-slot-assignedelements
Vector<GC::Root<DOM::Element>> HTMLSlotElement::assigned_elements(AssignedNodesFlatten flatten) const
{
    // 1. If options["flatten"] is false, then return this's assigned nodes, filtered to contain only Element nodes.
    if (flatten == AssignedNodesFlatten::No) {
        assign_slottables_if_needed();
        Vector<GC::Root<DOM::Element>> assigned_nodes;
        for (auto const& node : assigned_nodes_internal()) {
            if (auto* element = as_if<DOM::Element>(*node))
                assigned_nodes.append(*element);
        }
        return assigned_nodes;
    }

    // 2. Return the result of finding flattened slottables with this, filtered to contain only Element nodes.
    // NB: libweb_rust finds them, see Rust/src/dom/slots.rs.
    DOM::RootedNodeList result;
    DOM::RustFFI::rust_dom_find_flattened_slottables(this, &result.core());
    Vector<GC::Root<DOM::Element>> assigned_nodes;
    for (auto const& node : result.nodes()) {
        if (auto* element = as_if<DOM::Element>(*node))
            assigned_nodes.append(*element);
    }
    return assigned_nodes;
}

Vector<GC::Root<DOM::Element>> HTMLSlotElement::assigned_elements(Bindings::AssignedNodesOptions const& options) const
{
    return assigned_elements(assigned_nodes_flatten(options));
}

void HTMLSlotElement::assign_slottables_if_needed() const
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(const_cast<HTMLSlotElement&>(*this).root()))
        shadow_root->assign_slottables_if_needed();
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-slot-assign
void HTMLSlotElement::assign(GC::RootVector<SlottableHandle> nodes)
{
    // AD-HOC: Assigned nodes are recomputed lazily.
    assign_slottables_if_needed();

    // A name that comes from content can come from the nodes assigned to a slot.
    if (document().page().accessibility_interested())
        document().invalidate_cached_accessibility_data(*this);

    auto slottable_mixin_of = [](DOM::Node& node) -> DOM::SlottableMixin& {
        if (auto* element = as_if<DOM::Element>(node))
            return *element;
        return as<DOM::Text>(node);
    };

    // 1. For each node of this's manually assigned nodes, set node's manual slot assignment to null.
    for (auto const& node : manually_assigned_nodes())
        slottable_mixin_of(*node).set_manual_slot_assignment(nullptr);

    // 2. Let nodesSet be a new ordered set.
    GC::RootVector<GC::Ref<DOM::Node>> nodes_set;
    GC::RootHashTable<GC::Ref<DOM::Node const>> nodes_in_nodes_set;

    // INTEROP: The spec only assigns slottables for this's root below, so a slot in another tree that loses a node
    //          to this slot keeps it as an assigned node and fires no slotchange event. Browsers also assign
    //          slottables for the trees of those slots, and so do we.
    GC::RootVector<GC::Ref<DOM::Node>> roots_of_previous_slots;

    // 3. For each node of nodes:
    for (auto& node_handle : nodes) {
        GC::Ref<DOM::Node> node_ref = node_handle.visit([](auto& node) -> GC::Ref<DOM::Node> { return *node; });
        auto& node = slottable_mixin_of(*node_ref);

        // 1. If node's manual slot assignment refers to a slot, then remove node from that slot's manually assigned nodes.
        if (node.manual_slot_assignment() != nullptr) {
            auto& previous_slot = *node.manual_slot_assignment();
            DOM::RustFFI::rust_dom_slot_remove_manually_assigned_node(&previous_slot, node_ref.ptr());

            auto& previous_slot_root = previous_slot.root();
            if (&previous_slot_root != &root() && !roots_of_previous_slots.contains_slow(GC::Ref { previous_slot_root }))
                roots_of_previous_slots.append(previous_slot_root);
        }

        // 2. Set node's manual slot assignment to this.
        node.set_manual_slot_assignment(this);

        // 3. Append node to nodesSet.
        // NB: nodesSet is an ordered set, so a node that is already in it is not appended again.
        if (nodes_in_nodes_set.set(node_ref) == HashSetResult::InsertedNewEntry)
            nodes_set.append(node_ref);
    }

    // 4. Set this's manually assigned nodes to nodesSet.
    DOM::RustFFI::rust_dom_slot_set_manually_assigned_nodes(this, nodes_set.span());

    // 5. Run assign slottables for a tree for this's root.
    DOM::RustFFI::rust_dom_assign_slottables_for_a_tree(&root());

    // INTEROP: See roots_of_previous_slots above.
    for (auto const& previous_slot_root : roots_of_previous_slots)
        DOM::RustFFI::rust_dom_assign_slottables_for_a_tree(previous_slot_root.ptr());
}

}
