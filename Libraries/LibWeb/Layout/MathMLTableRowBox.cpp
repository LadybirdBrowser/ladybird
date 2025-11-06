/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLTableRowBox.h>
#include <LibWeb/MathML/MathMLTableRowElement.h>
#include <LibWeb/Painting/MathMLTableRowPaintable.h>

namespace Web::Layout {

MathMLTableRowBox::MathMLTableRowBox(DOM::Document& document, MathML::MathMLTableRowElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLTableRowBox::create_paintable() const
{
    return Painting::MathMLTableRowPaintable::create(*this);
}

}
