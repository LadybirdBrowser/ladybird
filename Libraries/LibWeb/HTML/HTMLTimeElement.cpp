/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTimeElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLTimeElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTimeElement);

HTMLTimeElement::HTMLTimeElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTimeElement::~HTMLTimeElement() = default;

}
