/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Utf16View.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementLayout.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// A new element of the class T with the node document `document` and the name `qualified_name`, as T's constructor
// makes it. libweb_rust allocates every element (see Rust/src/dom/element_objects.rs), and makes it whole, or makes its
// prefix, over which C++ then constructs the payload of T.
template<typename T>
GC::Ref<T> allocate_element(Document& document, QualifiedName const& qualified_name)
{
    auto& element = *RustFFI::rust_dom_create_element(&document, NodeClassTraits<T>::id, qualified_name.identity());
    if constexpr (element_class_has_payload<T>)
        return GC::CellConstructionHook<T>::construct_payload(element);
    else
        return static_cast<T&>(element);
}

WEB_API ErrorOr<FixedArray<Utf16FlyString>> valid_local_names_for_given_html_element_interface(Utf16View html_element_interface_name);
bool is_unknown_html_element(Utf16FlyString const& tag_name);

struct Default { };

WEB_API WebIDL::ExceptionOr<GC::Ref<Element>> create_element(Document&, Utf16FlyString local_name, Optional<Utf16FlyString> namespace_, Optional<Utf16FlyString> prefix = {}, Optional<Utf16FlyString> is = Optional<Utf16FlyString> {}, bool synchronous_custom_elements_flag = false, Variant<GC::Ptr<HTML::CustomElementRegistry>, Default> registry = Default {});

}
