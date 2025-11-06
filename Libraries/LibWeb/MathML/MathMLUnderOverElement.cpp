/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLUnderOverBox.h>
#include <LibWeb/MathML/MathMLUnderOverElement.h>
#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLUnderOverElement);

MathMLUnderOverElement::MathMLUnderOverElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLUnderOverElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLUnderOverBox>(document(), *this, move(style));
}

MathMLUnderOverElement::UnderOverType MathMLUnderOverElement::underover_type() const
{
    auto const& tag = local_name();
    if (tag == TagNames::munder)
        return UnderOverType::Under;
    if (tag == TagNames::mover)
        return UnderOverType::Over;
    if (tag == TagNames::munderover)
        return UnderOverType::UnderOver;

    // Default to under
    return UnderOverType::Under;
}

}
