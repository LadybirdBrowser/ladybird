/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLTableBox.h>
#include <LibWeb/MathML/MathMLTableElement.h>
#include <LibWeb/Painting/MathMLTablePaintable.h>

namespace Web::Layout {

MathMLTableBox::MathMLTableBox(DOM::Document& document, MathML::MathMLTableElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLTableBox::create_paintable() const
{
    return Painting::MathMLTablePaintable::create(*this);
}

}
