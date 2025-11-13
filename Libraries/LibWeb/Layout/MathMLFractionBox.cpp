/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLFractionBox.h>
#include <LibWeb/MathML/MathMLFractionElement.h>
#include <LibWeb/Painting/MathMLFractionPaintable.h>

namespace Web::Layout {

MathMLFractionBox::MathMLFractionBox(DOM::Document& document, MathML::MathMLFractionElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLFractionBox::create_paintable() const
{
    return Painting::MathMLFractionPaintable::create(*this);
}

}
