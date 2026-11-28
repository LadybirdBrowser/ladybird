/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ElementLayout.h>
#include <LibWeb/DOM/NodeClassHeadersGenerated.h>

namespace Web::DOM {

// The C++ half of the elements whose objects libweb_rust defines (see Rust/src/dom/element_objects.rs): the vtable
// pointers of their classes, and what C++ does with the members it keeps in the opaque slots of their prefixes and in
// their payloads.
struct RustElementObjectHooks {
    // Whether T is the C++ shell of a class of elements whose objects libweb_rust defines: one the object of its layout
    // class lays out whole, which C++ can neither allocate nor destroy. NB: Its destructor, which libweb_rust skips for
    //     an element whose slots hold nothing to release, is the defaulted one.
    template<typename T>
    static constexpr bool is_shell()
    {
        return !element_class_has_payload<T> && is_shell_of_rust_object<T, typename ElementLayout<T>::RustObject>;
    }

    // Whether T is a class of elements with a payload whose objects libweb_rust defines: one C++ can only construct over
    // the prefix libweb_rust made, unless it is abstract, and cannot allocate, whose cells LibGC can hold, and whose
    // initialize(), which DOM::allocate_element() leaves out as it does for every element whose objects libweb_rust
    // defines, is that of its layout class.
    template<typename T>
    static constexpr bool is_payload_class()
    {
        using Layout = typename ElementLayout<T>::Class;
        constexpr bool is_constructible = __is_abstract(T) || GC::CellConstructionHook<T>::constructs_over_rust_made_prefix;
        constexpr bool has_allocator = requires { requires IsSame<typename T::gc_allocator_marker, T>; };
        constexpr bool keeps_initialize = IsSame<decltype(&T::initialize), decltype(&Layout::initialize)>;
        return element_class_has_payload<T> && is_constructible && !has_allocator && alignof(T) == alignof(Layout) && keeps_initialize;
    }

    static Element& element_of(Node const* node)
    {
        return const_cast<Element&>(static_cast<Element const&>(*node));
    }

    // Visits the cells of an element whose class has traced members in its payload, through the visit_edges() of its
    // class.
    static void visit_edges(Node const* node, GCVisitor* gc_visitor)
    {
        element_of(node).visit_edges(*reinterpret_cast<GC::Cell::Visitor*>(gc_visitor));
    }

    // What DOM::Element::visit_edges() visits beyond what libweb_rust traces of an element.
    static void visit_slot_edges(Node const* node, GCVisitor* gc_visitor)
    {
        auto& element = element_of(node);
        auto& visitor = *reinterpret_cast<GC::Cell::Visitor*>(gc_visitor);
        element.visit_cxx_slot_edges(visitor);
        element.Animations::Animatable::visit_edges(visitor);
        visitor.visit(element.m_inline_style);
    }

    static void finalize(Node const* node)
    {
        element_of(node).finalize();
    }

    static size_t external_memory_size(Node const* node)
    {
        return element_of(node).external_memory_size();
    }

    // NB: libweb_rust releases what the element's core holds first, and leaves each of its fields as their destructors
    //     take an empty one to be.
    static void destroy(Node const* node)
    {
        element_of(node).~Element();
    }

    // A probe of the class T, which holds nothing that needs destroying, and so is never destroyed.
    template<typename T>
    static T const& probe()
    {
        alignas(T) static u8 storage[sizeof(T)];
        auto& probe = *new (storage) T(VtableProbe {});
        GC::CellConstructionHook<T>::did_construct(probe);
        return probe;
    }

    // The vtable pointer of the object or base class subobject `base` of `element`, which C++ writes at its start,
    // `offset` bytes into the element, where libweb_rust writes it.
    template<typename Base>
    static void const* vtable_pointer_of(Element const& element, Base const& base, size_t offset)
    {
        void const* address = &base;
        VERIFY(address == reinterpret_cast<u8 const*>(&element) + offset);
        void const* vtable_pointer = nullptr;
        __builtin_memcpy(&vtable_pointer, address, sizeof(vtable_pointer));
        return vtable_pointer;
    }

    // The vtable pointers that libweb_rust writes into the elements of the class T, which C++ writes itself into an
    // element with a payload as it constructs it.
    template<typename T>
    static RustFFI::ElementClassVtables vtables_of()
    {
        if constexpr (element_class_has_payload<T>)
            return {};
        else
            return vtables_of_probe<T>();
    }

    template<typename T>
    static RustFFI::ElementClassVtables vtables_of_probe()
    {
        auto const& element = probe<T>();
        RustFFI::ElementClassVtables vtables {
            .vtable = vtable_pointer_of(element, element, 0),
            .slottable_mixin = vtable_pointer_of<SlottableMixin>(element, element, offsetof(RustFFI::ElementPrefix, slottable_mixin_vtable)),
            .aria_mixin = vtable_pointer_of<ARIA::ARIAMixin>(element, element, offsetof(RustFFI::ElementPrefix, aria_mixin_vtable)),
            .animatable = vtable_pointer_of<Animations::Animatable>(element, element, offsetof(RustFFI::ElementPrefix, animatable_vtable)),
            .global_event_handlers = nullptr,
            .form_associated_element = nullptr,
            .probe = &element,
        };
        if constexpr (IsBaseOf<HTML::HTMLElement, T>) {
            vtables.global_event_handlers = vtable_pointer_of<HTML::GlobalEventHandlers>(element, element, offsetof(RustFFI::HTMLElementPrefix, global_event_handlers_vtable));
            vtables.form_associated_element = vtable_pointer_of<HTML::FormAssociatedElement>(element, element, offsetof(RustFFI::HTMLElementPrefix, form_associated_element_vtable));
        } else if constexpr (IsBaseOf<MathML::MathMLElement, T>) {
            vtables.global_event_handlers = vtable_pointer_of<HTML::GlobalEventHandlers>(element, element, offsetof(RustFFI::MathMLElementPrefix, global_event_handlers_vtable));
        }
        return vtables;
    }

    static RustFFI::ElementObjectHooks make()
    {
        static RustFFI::ElementClassVtables const classes[] = {
#define __ENUMERATE_RUST_OWNED_ELEMENT_CLASS(T) vtables_of<T>(),
            ENUMERATE_RUST_OWNED_ELEMENT_CLASSES(__ENUMERATE_RUST_OWNED_ELEMENT_CLASS)
#undef __ENUMERATE_RUST_OWNED_ELEMENT_CLASS
        };
        return {
            .classes = classes,
            .class_count = array_size(classes),
            .visit_edges = visit_edges,
            .visit_slot_edges = visit_slot_edges,
            .finalize = finalize,
            .external_memory_size = external_memory_size,
            .destroy = destroy,
        };
    }
};

#define __ENUMERATE_RUST_OWNED_ELEMENT_CLASS(T) \
    static_assert(RustElementObjectHooks::is_shell<T>() || RustElementObjectHooks::is_payload_class<T>(), #T " must be a shell, or a class with a payload that C++ only constructs over the prefix libweb_rust makes");
ENUMERATE_RUST_OWNED_ELEMENT_CLASSES(__ENUMERATE_RUST_OWNED_ELEMENT_CLASS)
#undef __ENUMERATE_RUST_OWNED_ELEMENT_CLASS

// NB: libweb_rust asserts the sizes and alignments of the objects of the layout classes.
static_assert(sizeof(Element) == sizeof(RustFFI::ElementObject) && alignof(Element) == alignof(RustFFI::ElementObject));
static_assert(sizeof(HTML::HTMLElement) == sizeof(RustFFI::HTMLElementObject) && alignof(HTML::HTMLElement) == alignof(RustFFI::HTMLElementObject));
static_assert(sizeof(MathML::MathMLElement) == sizeof(RustFFI::MathMLElementObject) && alignof(MathML::MathMLElement) == alignof(RustFFI::MathMLElementObject));

// NB: DOM/NodeObjects.cpp calls this as it registers its own hooks. Nothing else refers to this file, which a static
//     link would leave out if it registered the hooks itself.
void register_rust_element_object_hooks()
{
    static RustFFI::ElementObjectHooks const hooks = RustElementObjectHooks::make();
    RustFFI::rust_dom_register_element_objects(&hooks);
}

}
