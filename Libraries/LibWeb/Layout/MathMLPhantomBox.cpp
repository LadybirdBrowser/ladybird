/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLPhantomBox.h>
#include <LibWeb/MathML/MathMLPhantomElement.h>
#include <LibWeb/Painting/MathMLPhantomPaintable.h>

namespace Web::Layout {

MathMLPhantomBox::MathMLPhantomBox(DOM::Document& document, MathML::MathMLPhantomElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLPhantomBox::create_paintable() const
{
    return Painting::MathMLPhantomPaintable::create(*this);
}

}
