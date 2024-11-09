/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSlotElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLSlotElement);

HTMLSlotElement::HTMLSlotElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLSlotElement::~HTMLSlotElement() = default;

void HTMLSlotElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLSlotElement);
}

void HTMLSlotElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Slot::visit_edges(visitor);

    for (auto const& node : m_manually_assigned_nodes)
        node.visit([&](auto const& slottable) { visitor.visit(slottable); });
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-slot-assignednodes
Vector<JS::Handle<DOM::Node>> HTMLSlotElement::assigned_nodes(AssignedNodesOptions options) const
{
    // 1. If options["flatten"] is false, then return this's assigned nodes.
    if (!options.flatten) {
        Vector<JS::Handle<DOM::Node>> assigned_nodes;
        assigned_nodes.ensure_capacity(assigned_nodes_internal().size());

        for (auto const& node : assigned_nodes_internal()) {
            node.visit([&](auto const& node) {
                assigned_nodes.unchecked_append(*node);
            });
        }

        return assigned_nodes;
    }

    // FIXME: 2. Return the result of finding flattened slottables with this.
    return {};
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-slot-assignedelements
Vector<JS::Handle<DOM::Element>> HTMLSlotElement::assigned_elements(AssignedNodesOptions options) const
{
    // 1. If options["flatten"] is false, then return this's assigned nodes, filtered to contain only Element nodes.
    if (!options.flatten) {
        Vector<JS::Handle<DOM::Element>> assigned_nodes;

        for (auto const& node : assigned_nodes_internal()) {
            if (auto const* element = node.get_pointer<JS::NonnullGCPtr<DOM::Element>>())
                assigned_nodes.append(*element);
        }

        return assigned_nodes;
    }

    // FIXME: 2. Return the result of finding flattened slottables with this, filtered to contain only Element nodes.
    return {};
}

// https://html.spec.whatwg.org/multipage/scripting.html#dom-slot-assign
void HTMLSlotElement::assign(Vector<SlottableHandle> nodes)
{
    // 1. For each node of this's manually assigned nodes, set node's manual slot assignment to null.
    for (auto& node : m_manually_assigned_nodes) {
        node.visit([&](auto& node) {
            node->set_manual_slot_assignment(nullptr);
        });
    }

    // 2. Let nodesSet be a new ordered set.
    Vector<DOM::Slottable> nodes_set;

    // 3. For each node of nodes:
    for (auto& node_handle : nodes) {
        auto& node = node_handle.visit([](auto& node) -> DOM::SlottableMixin& { return *node; });
        auto slottable = node_handle.visit([](auto& node) { return node->as_slottable(); });

        // 1. If node's manual slot assignment refers to a slot, then remove node from that slot's manually assigned nodes.
        if (node.manual_slot_assignment() != nullptr) {
            m_manually_assigned_nodes.remove_all_matching([&](auto const& manually_assigned_node) {
                return slottable == manually_assigned_node;
            });
        }

        // 2. Set node's manual slot assignment to this.
        node.set_manual_slot_assignment(this);

        // 3. Append node to nodesSet.
        nodes_set.append(slottable);
    }

    // 4. Set this's manually assigned nodes to nodesSet.
    m_manually_assigned_nodes = move(nodes_set);

    // 5. Run assign slottables for a tree for this's root.
    assign_slottables_for_a_tree(root());
}

// https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-change-ext
void HTMLSlotElement::attribute_change_steps(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_change_steps(local_name, old_value, value, namespace_);

    // 1. If element is a slot, localName is name, and namespace is null, then:
    if (local_name == AttributeNames::name && !namespace_.has_value()) {
        // 1. If value is oldValue, then return.
        if (value == old_value)
            return;

        // 2. If value is null and oldValue is the empty string, then return.
        if (!value.has_value() && old_value == String {})
            return;

        // 3. If value is the empty string and oldValue is null, then return.
        if (value == String {} && !old_value.has_value())
            return;

        // 4. If value is null or the empty string, then set element’s name to the empty string.
        if (!value.has_value())
            set_slot_name({});
        // 5. Otherwise, set element’s name to value.
        else
            set_slot_name(*value);

        // 6. Run assign slottables for a tree with element’s root.
        DOM::assign_slottables_for_a_tree(root());
    }
}

}
