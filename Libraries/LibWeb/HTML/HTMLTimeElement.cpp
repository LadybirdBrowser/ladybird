/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTimeElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLTimeElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLTimeElement);

HTMLTimeElement::HTMLTimeElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

void HTMLTimeElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTimeElement);
}

HTMLTimeElement::~HTMLTimeElement() = default;

}
