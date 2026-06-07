/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGPatternBox.h>
#include <LibWeb/Painting/SVGPatternPaintable.h>

namespace Web::Layout {

SVGPatternBox::SVGPatternBox(DOM::Document& document, SVG::SVGPatternElement& element, CSS::ComputedProperties const& style)
    : SVGBox(document, element, style)
{
}

RefPtr<Painting::Paintable> SVGPatternBox::create_paintable() const
{
    return Painting::SVGPatternPaintable::create(*this);
}

}
