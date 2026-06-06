/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSpanElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLSpanElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLSpanElement);

HTMLSpanElement::HTMLSpanElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLSpanElement::~HTMLSpanElement() = default;

}
