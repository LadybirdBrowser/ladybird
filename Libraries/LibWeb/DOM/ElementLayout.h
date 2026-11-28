/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/MathML/MathMLElement.h>

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

// Whether an element of the class T has a payload: members, or overrides of the functions that make, trace, clone,
// finalize or measure an element, that come after the prefix of its layout class, which C++ constructs over the prefix
// libweb_rust made (see RustMadePrefix in DOM/Node.h). An element of a class without one is the object of its layout
// class, which libweb_rust makes whole.
template<typename T>
inline constexpr bool element_class_has_payload = [] {
    using Layout = typename ElementLayout<T>::Class;
    if constexpr (IsSame<T, Layout>)
        return false;
    else
        return !GC::CellConstructionHook<T>::template adds_nothing_to<Layout>();
}();

// Registers the hooks through which libweb_rust makes, traces and destroys the elements of every class, see
// DOM/ElementObjects.cpp.
void register_rust_element_object_hooks();

}
