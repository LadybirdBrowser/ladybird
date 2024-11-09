/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGPolylineElement final : public SVGGeometryElement {
    WEB_PLATFORM_OBJECT(SVGPolylineElement, SVGGeometryElement);
    JS_DECLARE_ALLOCATOR(SVGPolylineElement);

public:
    virtual ~SVGPolylineElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

private:
    SVGPolylineElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Vector<Gfx::FloatPoint> m_points;
};

}
