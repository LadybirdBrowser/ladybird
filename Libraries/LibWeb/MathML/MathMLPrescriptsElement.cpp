/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLPrescriptsBox.h>
#include <LibWeb/MathML/MathMLPrescriptsElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLPrescriptsElement);

MathMLPrescriptsElement::MathMLPrescriptsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLPrescriptsElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLPrescriptsBox>(document(), *this, move(style));
}

}
