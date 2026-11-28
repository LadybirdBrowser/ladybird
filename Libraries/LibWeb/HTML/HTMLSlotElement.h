/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::Bindings {

struct AssignedNodesOptions;

}

namespace Web::HTML {

// https://dom.spec.whatwg.org/#concept-slot
class HTMLSlotElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLSlotElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLSlotElement);

public:
    // NB: The buffers of the core are freed when the cell is swept, not when it is finalized, as the C++ members that
    //     held them were, so that the finalizer of another dead cell can still read them.
    GC_ALLOW_CELL_DESTRUCTOR virtual ~HTMLSlotElement() override;

    enum class AssignedNodesFlatten {
        No,
        Yes,
    };

    Vector<GC::Root<DOM::Node>> assigned_nodes(AssignedNodesFlatten = AssignedNodesFlatten::No) const;
    Vector<GC::Root<DOM::Node>> assigned_nodes(Bindings::AssignedNodesOptions const&) const;
    Vector<GC::Root<DOM::Element>> assigned_elements(AssignedNodesFlatten = AssignedNodesFlatten::No) const;
    Vector<GC::Root<DOM::Element>> assigned_elements(Bindings::AssignedNodesOptions const&) const;

    using SlottableHandle = Variant<GC::Ref<DOM::Element>, GC::Ref<DOM::Text>>;
    void assign(GC::RootVector<SlottableHandle> nodes);

    // https://dom.spec.whatwg.org/#slot-assigned-nodes, which libweb_rust keeps in the slot's core. NB: The list is
    // current only once assign_slottables_if_needed() has brought it up to date.
    ReadonlySpan<GC::Member<DOM::Node>> assigned_nodes_internal() const { return { m_slot_core.assigned_nodes.items, m_slot_core.assigned_nodes.size }; }

    // https://html.spec.whatwg.org/multipage/scripting.html#manually-assigned-nodes
    ReadonlySpan<GC::Member<DOM::Node>> manually_assigned_nodes() const { return { m_slot_core.manually_assigned_nodes.items, m_slot_core.manually_assigned_nodes.size }; }

    void assign_slottables_if_needed() const;

private:
    HTMLSlotElement(DOM::Document&, DOM::QualifiedName);

    // NB: The slot's name and its lists of nodes, see Rust/src/dom/slots.rs.
    DOM::RustFFI::SlotCore m_slot_core {};
};

}

namespace Web::DOM {

// NB: Only a slot is ever assigned to a slottable.
inline GC::Ptr<HTML::HTMLSlotElement> Element::assigned_slot_internal() const
{
    return static_cast<HTML::HTMLSlotElement*>(assigned_slot_in_core());
}

inline GC::Ptr<HTML::HTMLSlotElement> Text::assigned_slot_internal() const
{
    return static_cast<HTML::HTMLSlotElement*>(assigned_slot_in_core());
}

// NB: Elements and Text nodes keep their assigned slot in their cores. Ancestor walks ask this of every node they pass,
//     so it is inline and checks the node's flag before finding the core.
inline GC::Ptr<HTML::HTMLSlotElement> assigned_slot_for_node(GC::Ref<Node> node)
{
    if (!node->has_assigned_slot())
        return nullptr;
    if (auto const* element = as_if<Element>(*node))
        return element->assigned_slot_internal();
    if (auto const* text = as_if<Text>(*node))
        return text->assigned_slot_internal();
    return nullptr;
}

}
