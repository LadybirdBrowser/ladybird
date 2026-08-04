/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGEllipseElement final : public SVGGeometryElement {
    WEB_WRAPPABLE(SVGEllipseElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGEllipseElement);

public:
    virtual ~SVGEllipseElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    // AD-HOC: The spec states that the cx, cy, rx and ry IDL attributes reflect the respective computed values and their
    //         corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGEllipseElement__cx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(cx, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGEllipseElement__cy
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(cy, Vertical, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGEllipseElement__rx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(rx, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGEllipseElement__ry
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(ry, Vertical, CSS::NumberStyleValue::create(0));

private:
    SVGEllipseElement(DOM::Document&, DOM::QualifiedName);

    Optional<NumberPercentage> m_center_x;
    Optional<NumberPercentage> m_center_y;
    Optional<NumberPercentage> m_radius_x;
    Optional<NumberPercentage> m_radius_y;
};

}
