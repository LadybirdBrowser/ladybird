/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ElementLayout.h>
#include <LibWeb/DOM/NodeClassHeadersGenerated.h>

namespace Web::DOM {

// The C++ half of the nodes whose objects libweb_rust defines (see Rust/src/dom/node_objects.rs): the vtable pointers
// of their classes, and what C++ does with the members it keeps in the opaque slots of their prefixes.
struct RustNodeObjectHooks {
    static void visit_slot_edges(Node const* node, GCVisitor* visitor)
    {
        const_cast<Node&>(*node).visit_cxx_slot_edges(*reinterpret_cast<GC::Cell::Visitor*>(visitor));
    }

    static void finalize(Node const* node)
    {
        const_cast<Node&>(*node).finalize();
    }

    static size_t external_memory_size(Node const* node)
    {
        return node->external_memory_size();
    }

    // NB: The destructors of Node and of the classes it derives from destroy the members in the slots, and leave the
    //     fields Rust defines in their prefixes, which are trivially destructible, as they are.
    static_assert(IsTriviallyDestructible<RustFFI::NodeCore> && IsTriviallyDestructible<RustFFI::EventTargetCore> && IsTriviallyDestructible<NodeClassId>);
    static void destroy_slots(Node const* node)
    {
        const_cast<Node&>(*node).Node::~Node();
    }

    static void visit_edges(Node const* node, GCVisitor* visitor)
    {
        const_cast<Node&>(*node).visit_edges(*reinterpret_cast<GC::Cell::Visitor*>(visitor));
    }

    // NB: libweb_rust releases what the cores of the node's prefixes hold first, and leaves each of them as the
    //     destructors of their classes take it to be.
    static void destroy(Node const* node)
    {
        const_cast<Node&>(*node).~Node();
    }

    // A probe of the class T, which holds nothing that needs destroying, and so is never destroyed.
    template<typename T>
    static T const& probe()
    {
        alignas(T) static u8 storage[sizeof(T)];
        return *new (storage) T(VtableProbe {});
    }

    // The vtable pointer of the object or base class subobject at address, which C++ writes at its start.
    static void const* vtable_pointer_at(void const* address)
    {
        void const* vtable_pointer = nullptr;
        __builtin_memcpy(&vtable_pointer, address, sizeof(vtable_pointer));
        return vtable_pointer;
    }

    // NB: libweb_rust makes the copies of these nodes when it clones them, and runs no cloning steps for them.
    template<typename T>
    static constexpr bool has_no_cloning_steps = IsSame<decltype(&T::cloned), WebIDL::ExceptionOr<void> (Node::*)(Node&, bool) const>;
    static_assert(has_no_cloning_steps<Text> && has_no_cloning_steps<CDATASection> && has_no_cloning_steps<Comment> && has_no_cloning_steps<ProcessingInstruction> && has_no_cloning_steps<DocumentType> && has_no_cloning_steps<Attr>);

    // NB: A Text node's SlottableMixin base holds only its vtable pointer, which TextPrefix keeps in an opaque slot, and
    //     which Node::~Node() leaves as it is.
    static_assert(sizeof(SlottableMixin) == sizeof(void*));

    // The vtable pointer of the SlottableMixin base of the Text node `text`.
    static void const* slottable_mixin_vtable_pointer(Text const& text)
    {
        auto const* slottable_mixin = static_cast<SlottableMixin const*>(&text);
        VERIFY(reinterpret_cast<u8 const*>(slottable_mixin) == reinterpret_cast<u8 const*>(&text) + offsetof(RustFFI::TextPrefix, slottable_mixin_vtable));
        return vtable_pointer_at(slottable_mixin);
    }

    static RustFFI::NodeObjectHooks make()
    {
        auto const& text = probe<Text>();
        auto const& cdata_section = probe<CDATASection>();
        return {
            .text_vtable = vtable_pointer_at(&text),
            .text_slottable_mixin_vtable = slottable_mixin_vtable_pointer(text),
            .cdata_section_vtable = vtable_pointer_at(&cdata_section),
            .cdata_section_slottable_mixin_vtable = slottable_mixin_vtable_pointer(cdata_section),
            .comment_vtable = vtable_pointer_at(&probe<Comment>()),
            .processing_instruction_vtable = vtable_pointer_at(&probe<ProcessingInstruction>()),
            .document_type_vtable = vtable_pointer_at(&probe<DocumentType>()),
            .attr_vtable = vtable_pointer_at(&probe<Attr>()),
            .document_fragment_vtable = vtable_pointer_at(&probe<DocumentFragment>()),
            .visit_slot_edges = visit_slot_edges,
            .finalize = finalize,
            .external_memory_size = external_memory_size,
            .destroy_slots = destroy_slots,
            .visit_edges = visit_edges,
            .destroy = destroy,
        };
    }
};

// The layout class of the class T of nodes: the class whose behavior the object libweb_rust lays out for a node of T
// stands for, which is T itself for a class whose objects libweb_rust defines whole, and the layout class of an element
// (see DOM/ElementLayout.h). A shadow root is a DocumentFragment, and a document a ParentNode, with a payload.
template<typename T>
struct NodeLayout {
    using Class = T;
};

template<typename T>
requires(IsBaseOf<Element, T>)
struct NodeLayout<T> {
    using Class = typename ElementLayout<T>::Class;
};

template<>
struct NodeLayout<ShadowRoot> {
    using Class = DocumentFragment;
};

template<typename T>
requires(IsBaseOf<Document, T>)
struct NodeLayout<T> {
    using Class = ParentNode;
};

// What libweb_rust needs to know of the class T of nodes (see NodeClassFacts in Rust/src/dom/node_class.rs).
template<typename T>
static constexpr RustFFI::NodeClassFacts facts_of()
{
    using Layout = typename NodeLayout<T>::Class;
    u32 flags = 0;
    if constexpr (!IsSame<T, Layout>) {
        if constexpr (!GC::CellConstructionHook<T>::template adds_nothing_to<Layout>())
            flags = GC::CellConstructionHook<T>::template payload_flags<Layout>();
    }
    return { .cell_size = static_cast<u32>(sizeof(T)), .flags = flags };
}

// NB: An element of a class has a payload exactly where DOM::allocate_element() constructs one.
#define __ENUMERATE_RUST_OWNED_ELEMENT_CLASS(T) \
    static_assert(((facts_of<T>().flags & RustFFI::NODE_OBJECT_HAS_PAYLOAD) != 0) == element_class_has_payload<T>);
ENUMERATE_RUST_OWNED_ELEMENT_CLASSES(__ENUMERATE_RUST_OWNED_ELEMENT_CLASS)
#undef __ENUMERATE_RUST_OWNED_ELEMENT_CLASS

// The facts of each class of nodes, by class id, which libweb_rust reads as it registers the classes.
static consteval Array<RustFFI::NodeClassFacts, node_class_id_count> compute_node_class_facts()
{
    Array<RustFFI::NodeClassFacts, node_class_id_count> facts {};
#define __ENUMERATE_NODE_CLASS(T) \
    facts[to_underlying(NodeClassTraits<T>::id)] = facts_of<T>();
    ENUMERATE_NODE_CLASSES(__ENUMERATE_NODE_CLASS)
#undef __ENUMERATE_NODE_CLASS
    return facts;
}

extern "C" WEB_API Array<RustFFI::NodeClassFacts, node_class_id_count> const ladybird_dom_node_class_facts;
constinit Array<RustFFI::NodeClassFacts, node_class_id_count> const ladybird_dom_node_class_facts = compute_node_class_facts();

static RustFFI::NodeObjectHooks const s_rust_node_object_hooks = RustNodeObjectHooks::make();

static struct RustObjectHooksRegistration {
    RustObjectHooksRegistration()
    {
        RustFFI::rust_dom_register_node_objects(&s_rust_node_object_hooks);
        register_rust_element_object_hooks();
    }
} s_rust_object_hooks_registration;

}
