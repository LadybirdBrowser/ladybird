/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Layout {

SVGSVGBox::SVGSVGBox(DOM::Document& document, SVG::SVGSVGElement& element, CSS::LayoutStyle style)
    : ReplacedBox(document, element, style)
{
}

RefPtr<Painting::Paintable> SVGSVGBox::create_paintable() const
{
    return Painting::SVGSVGPaintable::create(*this);
}

CSS::SizeWithAspectRatio SVGSVGBox::natural_size() const
{
    auto metrics = SVG::SVGSVGElement::negotiate_natural_metrics(dom_node(), CSS::Length::ResolutionContext::for_layout_node(*this));
    return { metrics.width, metrics.height, metrics.aspect_ratio };
}

Gfx::FloatRect SVGSVGBox::view_box_or_viewport_rect() const
{
    // active_view_box covers <view> redirection and the svg-as-image fallback viewBox, which
    // layout used to build the geometry these callers interpret.
    if (auto view_box = dom_node().active_view_box(); view_box.has_value())
        return { view_box->min_x, view_box->min_y, view_box->width, view_box->height };
    if (auto const* paintable = as_if<Painting::SVGSVGPaintable>(paintable_box().ptr()))
        return { {}, { paintable->svg_viewport_size().width().to_float(), paintable->svg_viewport_size().height().to_float() } };
    return {};
}

}
