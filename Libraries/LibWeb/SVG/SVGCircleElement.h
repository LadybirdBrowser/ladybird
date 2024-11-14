/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGCircleElement final : public SVGGeometryElement {
    WEB_PLATFORM_OBJECT(SVGCircleElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGCircleElement);

public:
    virtual ~SVGCircleElement() override = default;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    GC::Ref<SVGAnimatedLength> cx() const;
    GC::Ref<SVGAnimatedLength> cy() const;
    GC::Ref<SVGAnimatedLength> r() const;

private:
    SVGCircleElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
