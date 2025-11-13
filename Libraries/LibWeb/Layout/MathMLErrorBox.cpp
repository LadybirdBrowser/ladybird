/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLErrorBox.h>
#include <LibWeb/MathML/MathMLErrorElement.h>
#include <LibWeb/Painting/MathMLErrorPaintable.h>

namespace Web::Layout {

MathMLErrorBox::MathMLErrorBox(DOM::Document& document, MathML::MathMLErrorElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLErrorBox::create_paintable() const
{
    return Painting::MathMLErrorPaintable::create(*this);
}

}
