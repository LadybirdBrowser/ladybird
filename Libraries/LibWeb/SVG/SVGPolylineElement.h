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
    GC_DECLARE_ALLOCATOR(SVGPolylineElement);

public:
    virtual ~SVGPolylineElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

private:
    SVGPolylineElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Vector<Gfx::FloatPoint> m_points;
};

}
