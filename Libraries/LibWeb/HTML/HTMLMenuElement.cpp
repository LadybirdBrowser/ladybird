/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLMenuElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLMenuElement);

HTMLMenuElement::HTMLMenuElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMenuElement::~HTMLMenuElement() = default;

}
