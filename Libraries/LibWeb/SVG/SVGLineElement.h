/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGLineElement final : public SVGGeometryElement {
    WEB_PLATFORM_OBJECT(SVGLineElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGLineElement);

public:
    virtual ~SVGLineElement() override = default;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    GC::Ref<SVGAnimatedLength> x1() const;
    GC::Ref<SVGAnimatedLength> y1() const;
    GC::Ref<SVGAnimatedLength> x2() const;
    GC::Ref<SVGAnimatedLength> y2() const;

private:
    SVGLineElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
