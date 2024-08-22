/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
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

    Optional<CSSPixels> natural_width;
    if (auto width = dom_node().width_style_value_from_attribute(); width && width->is_length() && width->as_length().length().is_absolute()) {
        natural_width = width->as_length().length().absolute_length_to_px();
    }

    Optional<CSSPixels> natural_height;
    if (auto height = dom_node().height_style_value_from_attribute(); height && height->is_length() && height->as_length().length().is_absolute()) {
        natural_height = height->as_length().length().absolute_length_to_px();
    }

    // The intrinsic aspect ratio must be calculated using the following algorithm. If the algorithm returns null, then there is no intrinsic aspect ratio.
    auto natural_aspect_ratio = [&]() -> Optional<CSSPixelFraction> {
        // 1. If the width and height sizing properties on the ‘svg’ element are both absolute values:
        if (natural_width.has_value() && natural_height.has_value()) {
            if (natural_width != 0 && natural_height != 0) {
                // 1. return width / height
                return *natural_width / *natural_height;
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
    }();

    set_natural_width(natural_width);
    set_natural_height(natural_height);
    set_natural_aspect_ratio(natural_aspect_ratio);
}

}
