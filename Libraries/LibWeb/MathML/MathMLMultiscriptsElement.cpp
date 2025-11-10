/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLMultiscriptsBox.h>
#include <LibWeb/MathML/MathMLMultiscriptsElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLMultiscriptsElement);

MathMLMultiscriptsElement::MathMLMultiscriptsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLMultiscriptsElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLMultiscriptsBox>(document(), *this, move(style));
}

}
