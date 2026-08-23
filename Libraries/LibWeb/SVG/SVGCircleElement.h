/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGCircleElement final : public SVGGeometryElement {
    WEB_WRAPPABLE(SVGCircleElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGCircleElement);

public:
    virtual ~SVGCircleElement() override = default;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size, CSS::ComputedValues const&) override;

    // AD-HOC: The spec states that the cx, cy and r IDL attributes reflect the respective computed values and their
    //         corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGCircleElement__cx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(cx, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGCircleElement__cy
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(cy, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGCircleElement__r
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(r, Unspecified, SVGLengthValue::number(0));

private:
    SVGCircleElement(DOM::Document&, DOM::QualifiedName);
};

}
