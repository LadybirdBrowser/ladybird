/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/shapes.html#RectElement
class SVGRectElement final : public SVGGeometryElement {
    WEB_WRAPPABLE(SVGRectElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGRectElement);

public:
    virtual ~SVGRectElement() override = default;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    // AD-HOC: The spec states that the x, y, width, height, rx and ry IDL attributes reflect the respective computed values
    //         and their corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__rx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(rx, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__ry
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(ry, Vertical, SVGLengthValue::number(0));

private:
    SVGRectElement(DOM::Document&, DOM::QualifiedName);

    Gfx::FloatSize calculate_used_corner_radius_values(float used_width, float used_height) const;
};

}
