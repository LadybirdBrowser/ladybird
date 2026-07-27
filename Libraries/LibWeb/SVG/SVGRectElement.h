/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/shapes.html#RectElement
class SVGRectElement final : public SVGGeometryElement {
    WEB_PLATFORM_OBJECT(SVGRectElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGRectElement);

public:
    virtual ~SVGRectElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    // AD-HOC: The spec states that the x, y, width, height, rx and ry IDL attributes reflect the respective computed values
    //         and their corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__rx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(rx, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/shapes.html#__svg__SVGRectElement__ry
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(ry, Vertical, CSS::NumberStyleValue::create(0));

private:
    SVGRectElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Gfx::FloatSize calculate_used_corner_radius_values(CSSPixelSize viewport_size) const;

    Optional<NumberPercentage> m_x;
    Optional<NumberPercentage> m_y;
    Optional<NumberPercentage> m_width;
    Optional<NumberPercentage> m_height;
    Optional<NumberPercentage> m_radius_x;
    Optional<NumberPercentage> m_radius_y;
};

}
