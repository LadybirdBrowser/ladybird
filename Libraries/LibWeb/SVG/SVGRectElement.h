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
    GC_DECLARE_ALLOCATOR(SVGRectElement);

public:
    virtual ~SVGRectElement() override = default;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> width() const;
    GC::Ref<SVGAnimatedLength> height() const;
    GC::Ref<SVGAnimatedLength> rx() const;
    GC::Ref<SVGAnimatedLength> ry() const;

private:
    SVGRectElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Gfx::FloatSize calculate_used_corner_radius_values(Layout::Node const&, CSSPixelSize viewport_size, float width, float height) const;
};

}
