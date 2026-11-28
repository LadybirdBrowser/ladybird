/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NodeClassHeadersGenerated.h>

namespace Web::DOM {

// The layout class of the class T of elements: the nearest class it derives from whose prefix lays out the elements of
// T in libweb_rust (see Rust/src/dom/prefix.rs), and the object Rust makes of each.
template<typename T>
struct ElementLayout {
    using Class = Element;
    using RustObject = RustFFI::ElementObject;
};

template<typename T>
requires(IsBaseOf<HTML::HTMLElement, T>)
struct ElementLayout<T> {
    using Class = HTML::HTMLElement;
    using RustObject = RustFFI::HTMLElementObject;
};

template<typename T>
requires(IsBaseOf<MathML::MathMLElement, T>)
struct ElementLayout<T> {
    using Class = MathML::MathMLElement;
    using RustObject = RustFFI::MathMLElementObject;
};

// The C++ half of the elements whose objects libweb_rust defines (see Rust/src/dom/element_objects.rs): the vtable
// pointers of their classes, and what C++ does with the members it keeps in the opaque slots of their prefixes.
struct RustElementObjectHooks {
    // Whether libweb_rust can define the objects of the class T of elements, as those of its layout class: T is a
    // layout class, or adds nothing to its layout class.
    template<typename T>
    static constexpr bool can_be_rust_owned()
    {
        using Layout = typename ElementLayout<T>::Class;
        if constexpr (IsSame<T, Layout>)
            return true;
        else
            return GC::CellConstructionHook<T>::template adds_nothing_to<Layout>();
    }

    // Whether T is the C++ shell of a class of elements whose objects libweb_rust defines: one the object of its layout
    // class lays out whole, which C++ can neither allocate nor destroy. NB: Its destructor, which libweb_rust skips for
    //     an element whose slots hold nothing to release, is the defaulted one.
    template<typename T>
    static constexpr bool is_shell()
    {
        return can_be_rust_owned<T>() && is_shell_of_rust_object<T, typename ElementLayout<T>::RustObject>;
    }

    static Element& element_of(Node const* node)
    {
        return const_cast<Element&>(static_cast<Element const&>(*node));
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

    template<typename T>
    static RustFFI::ElementClassVtables vtables_of()
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
            .visit_slot_edges = visit_slot_edges,
            .finalize = finalize,
            .external_memory_size = external_memory_size,
            .destroy = destroy,
        };
    }
};

#define __ENUMERATE_RUST_OWNED_ELEMENT_CLASS(T) \
    static_assert(RustElementObjectHooks::is_shell<T>(), #T " is a class of elements whose objects libweb_rust defines, see DOM/NodeClasses.json");
ENUMERATE_RUST_OWNED_ELEMENT_CLASSES(__ENUMERATE_RUST_OWNED_ELEMENT_CLASS)
#undef __ENUMERATE_RUST_OWNED_ELEMENT_CLASS

// NB: DOM/NodeClasses.json marks every class of elements whose objects libweb_rust can define, so a class that comes to
//     add nothing to its layout class becomes one.
template<typename T>
consteval bool is_rust_owned_exactly_where_it_can_be()
{
    if constexpr (IsBaseOf<Element, T>)
        return NodeClassTraits<T>::rust_owned == RustElementObjectHooks::can_be_rust_owned<T>();
    else
        return !NodeClassTraits<T>::rust_owned;
}

#define __ENUMERATE_NODE_CLASS(T) \
    static_assert(is_rust_owned_exactly_where_it_can_be<T>(), #T " must be marked \"@rust_owned\" in DOM/NodeClasses.json exactly where it adds nothing to its layout class");
ENUMERATE_NODE_CLASSES(__ENUMERATE_NODE_CLASS)
#undef __ENUMERATE_NODE_CLASS

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
