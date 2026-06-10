/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLParamElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLParamElement);

HTMLParamElement::HTMLParamElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLParamElement::~HTMLParamElement() = default;

}
