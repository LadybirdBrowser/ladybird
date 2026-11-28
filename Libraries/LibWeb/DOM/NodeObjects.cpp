/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/ProcessingInstruction.h>

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
        };
    }
};

static RustFFI::NodeObjectHooks const s_rust_node_object_hooks = RustNodeObjectHooks::make();

static struct RustObjectHooksRegistration {
    RustObjectHooksRegistration()
    {
        RustFFI::rust_dom_register_node_objects(&s_rust_node_object_hooks);
        register_rust_element_object_hooks();
    }
} s_rust_object_hooks_registration;

}
