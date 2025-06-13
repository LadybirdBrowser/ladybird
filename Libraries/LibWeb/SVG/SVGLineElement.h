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

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

    GC::Ref<SVGAnimatedLength> x1() const;
    GC::Ref<SVGAnimatedLength> y1() const;
    GC::Ref<SVGAnimatedLength> x2() const;
    GC::Ref<SVGAnimatedLength> y2() const;

private:
    SVGLineElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Optional<NumberPercentage> m_x1;
    Optional<NumberPercentage> m_y1;
    Optional<NumberPercentage> m_x2;
    Optional<NumberPercentage> m_y2;
};

}
