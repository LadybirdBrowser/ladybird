/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MathMLAnchorElement.h>
#include <LibWeb/MathML/MathMLAnchorElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnchorElement);

MathMLAnchorElement::MathMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

MathMLAnchorElement::~MathMLAnchorElement() = default;

}
