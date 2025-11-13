/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLMultiscriptsBox.h>
#include <LibWeb/MathML/MathMLMultiscriptsElement.h>
#include <LibWeb/Painting/MathMLMultiscriptsPaintable.h>

namespace Web::Layout {

MathMLMultiscriptsBox::MathMLMultiscriptsBox(DOM::Document& document, MathML::MathMLMultiscriptsElement& element, GC::Ref<CSS::ComputedProperties> style)
    : MathMLBox(document, element, move(style))
{
}

GC::Ptr<Painting::Paintable> MathMLMultiscriptsBox::create_paintable() const
{
    return Painting::MathMLMultiscriptsPaintable::create(*this);
}

}
