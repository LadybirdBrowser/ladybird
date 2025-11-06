/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLTableCellBox.h>
#include <LibWeb/MathML/MathMLTableCellElement.h>
#include <LibWeb/Painting/MathMLTableCellPaintable.h>

namespace Web::Layout {

MathMLTableCellBox::MathMLTableCellBox(DOM::Document& document, MathML::MathMLTableCellElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLTableCellBox::create_paintable() const
{
    return Painting::MathMLTableCellPaintable::create(*this);
}

}
