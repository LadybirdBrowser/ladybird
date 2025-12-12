/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLRadicalBox.h>
#include <LibWeb/Painting/MathMLRadicalPaintable.h>

namespace Web::Layout {

MathMLRadicalBox::MathMLRadicalBox(DOM::Document& document, MathML::MathMLElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLRadicalBox::create_paintable() const
{
    return Painting::MathMLRadicalPaintable::create(*this);
}

}
