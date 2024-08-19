/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLFrameElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLFrameElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFrameElement);

HTMLFrameElement::HTMLFrameElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFrameElement::~HTMLFrameElement() = default;

void HTMLFrameElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFrameElement);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLFrameElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

}
