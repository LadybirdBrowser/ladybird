/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLAnnotationBox.h>
#include <LibWeb/MathML/MathMLAnnotationElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnnotationElement);

MathMLAnnotationElement::MathMLAnnotationElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLAnnotationElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLAnnotationBox>(document(), *this, move(style));
}

}
