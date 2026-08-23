/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGLineElement final : public SVGGeometryElement {
    WEB_WRAPPABLE(SVGLineElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGLineElement);

public:
    virtual ~SVGLineElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size, CSS::ComputedValues const&) override;

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGLineElement__x1
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x1, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGLineElement__y1
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y1, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGLineElement__x2
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x2, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGLineElement__y2
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y2, Vertical, SVGLengthValue::number(0));

private:
    SVGLineElement(DOM::Document&, DOM::QualifiedName);

    Optional<NumberPercentage> m_x1;
    Optional<NumberPercentage> m_y1;
    Optional<NumberPercentage> m_x2;
    Optional<NumberPercentage> m_y2;
};

}
