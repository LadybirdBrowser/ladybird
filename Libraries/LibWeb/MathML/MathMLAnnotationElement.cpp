/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MathML/MathMLAnnotationElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnnotationElement);

MathMLAnnotationElement::MathMLAnnotationElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLAnnotationElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    (void)style;
    // Annotation elements are metadata only and must not participate in layout.
    return nullptr;
}

}
