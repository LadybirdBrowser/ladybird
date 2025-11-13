/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLUnderOverBox.h>
#include <LibWeb/MathML/MathMLUnderOverElement.h>
#include <LibWeb/Painting/MathMLUnderOverPaintable.h>

namespace Web::Layout {

MathMLUnderOverBox::MathMLUnderOverBox(DOM::Document& document, MathML::MathMLUnderOverElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLUnderOverBox::create_paintable() const
{
    return Painting::MathMLUnderOverPaintable::create(*this);
}

}
