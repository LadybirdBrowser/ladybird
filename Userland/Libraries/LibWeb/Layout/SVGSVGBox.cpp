/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(SVGSVGBox);

SVGSVGBox::SVGSVGBox(DOM::Document& document, SVG::SVGSVGElement& element, NonnullRefPtr<CSS::StyleProperties> properties)
    : ReplacedBox(document, element, move(properties))
{
}

JS::GCPtr<Painting::Paintable> SVGSVGBox::create_paintable() const
{
    return Painting::SVGSVGPaintable::create(*this);
}

void SVGSVGBox::prepare_for_replaced_layout()
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS

    // The intrinsic dimensions must also be determined from the width and height sizing properties.
    // If either width or height are not specified, the used value is the initial value 'auto'.
    // 'auto' and percentage lengths must not be used to determine an intrinsic width or intrinsic height.
    auto const& computed_width = computed_values().width();
    if (computed_width.is_length() && !computed_width.contains_percentage()) {
        set_natural_width(computed_width.to_px(*this, 0));
    }

    auto const& computed_height = computed_values().height();
    if (computed_height.is_length() && !computed_height.contains_percentage()) {
        set_natural_height(computed_height.to_px(*this, 0));
    }

    set_natural_aspect_ratio(calculate_intrinsic_aspect_ratio());
}

Optional<CSSPixelFraction> SVGSVGBox::calculate_intrinsic_aspect_ratio() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    // The intrinsic aspect ratio must be calculated using the following algorithm. If the algorithm returns null, then there is no intrinsic aspect ratio.

    auto const& computed_width = computed_values().width();
    auto const& computed_height = computed_values().height();

    // 1. If the width and height sizing properties on the ‘svg’ element are both absolute values:
    if (computed_width.is_length() && !computed_width.contains_percentage() && computed_height.is_length() && !computed_height.contains_percentage()) {
        auto width = computed_width.to_px(*this, 0);
        auto height = computed_height.to_px(*this, 0);

        if (width != 0 && height != 0) {
            // 1. return width / height
            return width / height;
        }

        return {};
    }

    // FIXME: 2. If an SVG View is active:
    // FIXME:    1. let viewbox be the viewbox defined by the active SVG View
    // FIXME:    2. return viewbox.width / viewbox.height

    // 3. If the ‘viewBox’ on the ‘svg’ element is correctly specified:
    if (dom_node().view_box().has_value()) {
        // 1. let viewbox be the viewbox defined by the ‘viewBox’ attribute on the ‘svg’ element
        auto const& viewbox = dom_node().view_box().value();

        // 2. return viewbox.width / viewbox.height
        auto viewbox_width = CSSPixels::nearest_value_for(viewbox.width);
        auto viewbox_height = CSSPixels::nearest_value_for(viewbox.height);
        if (viewbox_width != 0 && viewbox_height != 0)
            return viewbox_width / viewbox_height;

        return {};
    }

    // 4. return null
    return {};
}

}
