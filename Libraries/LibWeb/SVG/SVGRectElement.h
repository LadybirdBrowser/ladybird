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
    WEB_PLATFORM_OBJECT(SVGRectElement, SVGGeometryElement);
    JS_DECLARE_ALLOCATOR(SVGRectElement);

public:
    virtual ~SVGRectElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    JS::NonnullGCPtr<SVGAnimatedLength> x() const;
    JS::NonnullGCPtr<SVGAnimatedLength> y() const;
    JS::NonnullGCPtr<SVGAnimatedLength> width() const;
    JS::NonnullGCPtr<SVGAnimatedLength> height() const;
    JS::NonnullGCPtr<SVGAnimatedLength> rx() const;
    JS::NonnullGCPtr<SVGAnimatedLength> ry() const;

private:
    SVGRectElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Gfx::FloatSize calculate_used_corner_radius_values() const;

    Optional<float> m_x;
    Optional<float> m_y;
    Optional<float> m_width;
    Optional<float> m_height;
    Optional<float> m_radius_x;
    Optional<float> m_radius_y;
};

}
