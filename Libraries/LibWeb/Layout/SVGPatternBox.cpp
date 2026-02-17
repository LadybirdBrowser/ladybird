/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGPatternBox.h>
#include <LibWeb/Painting/SVGPatternPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(SVGPatternBox);

SVGPatternBox::SVGPatternBox(DOM::Document& document, SVG::SVGPatternElement& element, GC::Ref<CSS::ComputedProperties> style)
    : SVGBox(document, element, style)
{
}

GC::Ptr<Painting::Paintable> SVGPatternBox::create_paintable() const
{
    return Painting::SVGPatternPaintable::create(*this);
}

}
