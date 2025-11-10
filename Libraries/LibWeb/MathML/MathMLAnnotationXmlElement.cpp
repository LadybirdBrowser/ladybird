/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MathML/MathMLAnnotationXmlElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnnotationXmlElement);

MathMLAnnotationXmlElement::MathMLAnnotationXmlElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLAnnotationXmlElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    (void)style;
    // Annotation-XML elements are metadata only and must not participate in layout.
    return nullptr;
}

}
