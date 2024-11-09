/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLUnknownElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLUnknownElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLUnknownElement);

HTMLUnknownElement::HTMLUnknownElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLUnknownElement::~HTMLUnknownElement() = default;

void HTMLUnknownElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLUnknownElement);
}

}
