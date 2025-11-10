/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLRadicalBox.h>
#include <LibWeb/MathML/MathMLRadicalElement.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLRadicalElement);

MathMLRadicalElement::MathMLRadicalElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLRadicalElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLRadicalBox>(document(), *this, move(style));
}

}
